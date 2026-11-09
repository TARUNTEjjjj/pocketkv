#include "pocketkv/store.hpp"
#include "../src/format.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using pocketkv::Error;
using pocketkv::ErrorCode;
using pocketkv::Store;
using Bytes = std::vector<std::uint8_t>;

std::size_t assertions = 0;

void require(bool condition, std::string_view description) {
    ++assertions;
    if (!condition) throw std::runtime_error(std::string(description));
}

template<class Function>
void expect_error(ErrorCode code, Function function) {
    ++assertions;
    try { function(); }
    catch (const Error& error) {
        if (error.code() == code) return;
        throw std::runtime_error("Wrong PocketKV error code: " + std::string(error.what()));
    }
    throw std::runtime_error("Expected PocketKV error was not thrown");
}

struct Temp {
    fs::path directory;
    Temp() {
        auto pattern = (fs::temp_directory_path() / "pocketkv-test-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        char* result = ::mkdtemp(buffer.data());
        if (!result) throw std::runtime_error("mkdtemp failed");
        directory = result;
    }
    ~Temp() { std::error_code ignored; fs::remove_all(directory, ignored); }
    [[nodiscard]] fs::path path(std::string_view name = "data.db") const { return directory / name; }
};

Bytes read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Unable to read test fixture");
    std::vector<char> chars{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    return Bytes(chars.begin(), chars.end());
}

void write(const fs::path& path, const Bytes& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("Unable to write test fixture");
}

// Independent fixture encoder: this spells out the on-disk format rather than
// serializing through the production codec. The standard CRC check value below
// guards this helper against changing alongside the production implementation.
std::uint32_t reference_crc(std::span<const std::uint8_t> bytes) {
    std::uint32_t state = 0xffffffffU;
    for (const auto byte : bytes) {
        state ^= byte;
        for (unsigned bit = 0; bit != 8; ++bit) {
            state = (state >> 1U) ^ ((state & 1U) != 0U ? 0xedb88320U : 0U);
        }
    }
    return ~state;
}

void put_u32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i != 4; ++i) bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (8U * i));
}

Bytes external_header() {
    Bytes out{'P','O','C','K','E','T','K','V',1,0,0,0,0,0,0,0};
    put_u32(out, 12, reference_crc(std::span(out).first(12)));
    return out;
}

Bytes external_record(std::uint8_t kind, std::string_view key, std::string_view value) {
    Bytes payload(key.begin(), key.end());
    payload.insert(payload.end(), value.begin(), value.end());
    Bytes out{'P','K','V','R',kind,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    put_u32(out, 8, static_cast<std::uint32_t>(key.size()));
    put_u32(out, 12, static_cast<std::uint32_t>(value.size()));
    put_u32(out, 16, reference_crc(payload));
    put_u32(out, 20, reference_crc(std::span(out).first(20)));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

void append(Bytes& target, const Bytes& source) { target.insert(target.end(), source.begin(), source.end()); }

void child_success(pid_t child) {
    require(child > 0, "fork succeeded");
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) throw std::runtime_error("waitpid failed");
    }
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child operation succeeded");
}

void persistence() {
    Temp temp;
    {
        Store db(temp.path());
        require(db.stats().file_bytes == 16, "new database contains only header");
        require(db.keys().empty(), "new database is empty");
        require(!db.get("missing"), "missing key returns nullopt");
        db.put("z", "one");
        db.put("a", "");
        db.put("z", "two");
        require(db.keys() == std::vector<std::string>({"a", "z"}), "keys are sorted");
        const auto before = db.stats();
        require(!db.erase("missing"), "missing erase reports false");
        require(db.stats().record_count == before.record_count && db.stats().file_bytes == before.file_bytes, "missing erase has no append");
        require(db.erase("a"), "existing key erased");
        const auto stats = db.stats();
        require(stats.key_count == 1 && stats.record_count == 4, "latest keys and total records differ");
        require(stats.live_value_bytes == 3, "live values exclude deleted and superseded data");
        require(stats.file_bytes == 122, "exact record-format byte count");
    }
    {
        Store db(temp.path(), {.create_if_missing=false});
        require(db.get("z") == "two", "latest value survives reopen");
        require(!db.get("a"), "tombstone survives reopen");
        require(db.stats().record_count == 4 && db.stats().file_bytes == 122, "stats rebuilt from records");
        require(db.repaired_bytes() == 0, "valid database needs no repair");
    }
    expect_error(ErrorCode::io, [&] { Store missing(temp.path("missing"), {.create_if_missing=false}); });
    require(!fs::exists(temp.path("missing")), "read-only intent never creates missing database");
}

void binary_and_limits() {
    Temp temp;
    const std::string key("a\0b\xff", 4);
    const std::string value("x\0y\n\xff", 5);
    {
        Store db(temp.path());
        db.put(key, value);
        db.put("empty", "");
        require(db.get(key) == value, "binary key and value round trip");
        require(db.get("empty") == "", "empty value differs from absent key");
        expect_error(ErrorCode::invalid_argument, [&] { db.put("", "v"); });
        expect_error(ErrorCode::invalid_argument, [&] { (void)db.get(""); });
        expect_error(ErrorCode::invalid_argument, [&] { (void)db.erase(""); });
        const auto before = read(temp.path());
        expect_error(ErrorCode::invalid_argument, [&] { db.put(std::string(Store::max_key_bytes + 1, 'k'), "v"); });
        expect_error(ErrorCode::invalid_argument, [&] { db.put("big", std::string(Store::max_value_bytes + 1, 'v')); });
        require(read(temp.path()) == before, "invalid arguments never modify file");
        const std::string maximum_key(Store::max_key_bytes, 'k');
        const std::string maximum_value(Store::max_value_bytes, 'v');
        db.put(maximum_key, maximum_value);
        require(db.get(maximum_key) == maximum_value, "inclusive size boundaries work");
    }
    Store db(temp.path(), {.create_if_missing=false});
    require(db.get(key) == value, "binary values survive reopen");
    const auto largest = db.get(std::string(Store::max_key_bytes, 'k'));
    require(largest && largest->size() == Store::max_value_bytes, "largest value survives reopen");
}

void codec_and_external_fixture() {
    const Bytes check{'1','2','3','4','5','6','7','8','9'};
    require(reference_crc(check) == 0xcbf43926U, "reference IEEE CRC32 check value");
    require(pocketkv::detail::crc32(check) == 0xcbf43926U, "production IEEE CRC32 check value");
    require(pocketkv::detail::crc32({}) == 0, "CRC of empty payload");
    const auto generated = pocketkv::detail::make_file_header();
    require(Bytes(generated.begin(), generated.end()) == external_header(), "independent file-header encoding");
    Temp temp;
    auto fixture = external_header();
    append(fixture, external_record(1, "alpha", "old"));
    append(fixture, external_record(1, "beta", "kept"));
    append(fixture, external_record(1, "alpha", "new"));
    append(fixture, external_record(2, "beta", ""));
    write(temp.path(), fixture);
    Store db(temp.path(), {.create_if_missing=false});
    require(db.get("alpha") == "new" && !db.get("beta"), "external format fixture loads correctly");
    require(db.stats().record_count == 4, "external fixture record count");
}

void map_oracle() {
    Temp temp;
    std::map<std::string, std::string> expected;
    std::mt19937 rng(0x504b5654U);
    std::uint64_t records = 0;
    std::uint64_t bytes = 16;
    for (unsigned batch = 0; batch != 8; ++batch) {
        Store db(temp.path());
        for (unsigned i = 0; i != 40; ++i) {
            const auto key = "key-" + std::to_string(rng() % 23);
            if (rng() % 3 == 0) {
                const bool existed = expected.erase(key) != 0;
                require(db.erase(key) == existed, "erase agrees with map oracle");
                if (existed) { ++records; bytes += 24 + key.size(); }
            } else {
                const auto value = "value-" + std::to_string(rng());
                db.put(key, value);
                expected[key] = value;
                ++records;
                bytes += 24 + key.size() + value.size();
            }
            std::vector<std::string> keys;
            std::uint64_t live = 0;
            for (const auto& [name, value] : expected) {
                keys.push_back(name);
                live += value.size();
                require(db.get(name) == value, "get agrees with map oracle");
            }
            const auto stats = db.stats();
            require(db.keys() == keys, "keys agree with map oracle");
            require(stats.key_count == expected.size() && stats.record_count == records && stats.file_bytes == bytes && stats.live_value_bytes == live, "stats agree with independent operation ledger");
        }
    }
}

void partial_tail_recovery() {
    Temp temp;
    auto base = external_header();
    append(base, external_record(1, "stable", "value"));
    const std::array<Bytes, 2> tails{external_record(1, "fresh", "new-value"), external_record(2, "stable", "")};
    for (const auto& tail : tails) {
        for (std::size_t cut = 1; cut < tail.size(); ++cut) {
            auto damaged = base;
            damaged.insert(damaged.end(), tail.begin(), tail.begin() + static_cast<std::ptrdiff_t>(cut));
            write(temp.path(), damaged);
            expect_error(ErrorCode::incomplete_tail, [&] { Store db(temp.path(), {.create_if_missing=false}); });
            require(read(temp.path()) == damaged, "strict incomplete-tail rejection preserves all bytes");
            {
                Store db(temp.path(), {.create_if_missing=false, .repair_incomplete_tail=true});
                require(db.repaired_bytes() == cut, "repair reports exact discarded suffix length");
                require(read(temp.path()) == base, "repair truncates exactly to previous record boundary");
                require(db.get("stable") == "value" && !db.get("fresh"), "repair preserves previous records");
                require(db.stats().record_count == 1, "partial record never enters index");
                db.put("after", "recovery");
            }
            Store reopened(temp.path(), {.create_if_missing=false});
            require(reopened.get("stable") == "value" && reopened.get("after") == "recovery", "new append works after repair and reopen");
        }
        auto complete = base;
        append(complete, tail);
        write(temp.path(), complete);
        Store db(temp.path(), {.create_if_missing=false, .repair_incomplete_tail=true});
        require(db.repaired_bytes() == 0 && read(temp.path()) == complete, "complete final record is never truncated");
    }
    write(temp.path(), base);
    Store exact_boundary(temp.path(), {.create_if_missing=false});
    require(exact_boundary.get("stable") == "value", "exact record boundary is valid EOF");
}

void partial_length_validation() {
    Temp temp;
    auto base = external_header();
    append(base, external_record(1, "stable", "value"));

    struct InvalidLength {
        std::uint8_t kind;
        std::size_t field;
        std::uint32_t value;
        std::size_t cut;
    };
    const std::array<InvalidLength, 13> invalid{{
        {1, 8, 0, 10}, {1, 8, 0, 11}, {1, 8, 0, 12},
        {1, 8, 4097, 10}, {1, 8, 4097, 11}, {1, 8, 65535, 10},
        {1, 8, 65536, 11}, {1, 8, 0xffffff, 11},
        {1, 12, 1048577, 15}, {1, 12, 0xffffff, 15},
        {2, 12, 1, 13}, {2, 12, 256, 14}, {2, 12, 65536, 15},
    }};
    for (const auto& test : invalid) {
        auto tail = external_record(test.kind, "k", "");
        put_u32(tail, test.field, test.value);
        tail.resize(test.cut); // The checksum is absent; present length bytes suffice.
        auto damaged = base;
        append(damaged, tail);
        for (const bool repair : {false, true}) {
            write(temp.path(), damaged);
            expect_error(ErrorCode::corruption, [&] {
                Store db(temp.path(), {.create_if_missing=false, .repair_incomplete_tail=repair});
            });
            require(read(temp.path()) == damaged, "impossible partial length must preserve the entire file");
        }
    }

    // These independently encoded records exercise carries between length
    // bytes and the inclusive limits. A single zero key-length byte remains
    // valid because a later byte can complete it to 256 or 4096.
    std::vector<Bytes> valid;
    for (const auto& [key_length, value_length] :
         std::array<std::pair<std::size_t, std::size_t>, 6>{{
             {1, 0}, {255, 255}, {256, 256}, {4095, 65535},
             {4096, 65536}, {4096, Store::max_value_bytes}}}) {
        valid.push_back(external_record(1, std::string(key_length, 'k'),
                                         std::string(value_length, 'v')));
    }
    valid.push_back(external_record(2, std::string(256, 'k'), ""));
    for (const auto& tail : valid) {
        for (std::size_t cut = 1; cut < 24; ++cut) {
            auto incomplete = base;
            incomplete.insert(incomplete.end(), tail.begin(),
                              tail.begin() + static_cast<std::ptrdiff_t>(cut));
            write(temp.path(), incomplete);
            expect_error(ErrorCode::incomplete_tail, [&] {
                Store db(temp.path(), {.create_if_missing=false});
            });
            require(read(temp.path()) == incomplete, "valid partial header is preserved by a normal open");
            Store repaired(temp.path(), {.create_if_missing=false, .repair_incomplete_tail=true});
            require(repaired.repaired_bytes() == cut && read(temp.path()) == base,
                    "every valid boundary-length header prefix remains repairable");
            require(repaired.get("stable") == "value", "repair of a valid prefix preserves earlier data");
        }
    }
}

void corruption_is_not_repair() {
    Temp temp;
    auto header = external_header();
    auto record = external_record(1, "key", "value");
    auto original = header;
    append(original, record);
    std::vector<Bytes> malformed;
    for (const auto offset : {std::size_t(0), std::size_t(8), std::size_t(12), std::size_t(16), std::size_t(32), original.size() - 1}) {
        auto copy = original;
        copy[offset] ^= 0x80;
        malformed.push_back(copy);
    }
    for (const auto& [field, value] : std::array<std::pair<std::size_t, std::uint32_t>, 3>{{{8, 0}, {8, 4097}, {12, 1048577}}}) {
        auto bad = record;
        put_u32(bad, field, value);
        put_u32(bad, 20, reference_crc(std::span(bad).first(20)));
        auto copy = header; append(copy, bad); malformed.push_back(copy);
    }
    for (const auto& [offset, value] : std::array<std::pair<std::size_t, std::uint8_t>, 3>{{{4, 9}, {5, 1}, {6, 1}}}) {
        auto bad = record;
        bad[offset] = value;
        put_u32(bad, 20, reference_crc(std::span(bad).first(20)));
        auto copy = header; append(copy, bad); malformed.push_back(copy);
    }
    auto future = original;
    put_u32(future, 8, 2);
    put_u32(future, 12, reference_crc(std::span(future).first(12)));
    malformed.push_back(future);
    auto bad_delete = record;
    bad_delete[4] = 2;
    put_u32(bad_delete, 20, reference_crc(std::span(bad_delete).first(20)));
    auto deleted_value = header; append(deleted_value, bad_delete); malformed.push_back(deleted_value);
    auto garbage = original; append(garbage, Bytes{'n','o','t','a','r','e','c','o','r','d'}); malformed.push_back(garbage);
    for (std::size_t cut = 0; cut < header.size(); ++cut) malformed.emplace_back(header.begin(), header.begin() + static_cast<std::ptrdiff_t>(cut));
    for (const auto& bad : malformed) {
        for (const bool repair : {false, true}) {
            write(temp.path(), bad);
            expect_error(ErrorCode::corruption, [&] { Store db(temp.path(), {.create_if_missing=false, .repair_incomplete_tail=repair}); });
            require(read(temp.path()) == bad, "corrupt database remains byte-for-byte unchanged");
        }
    }
    // Failed constructors must release their descriptor/lock.
    write(temp.path(), original);
    Store db(temp.path(), {.create_if_missing=false});
    require(db.get("key") == "value", "valid file opens after repeated failed constructors");
    auto changed = original; changed.back() ^= 1;
    write(temp.path(), changed);
    expect_error(ErrorCode::corruption, [&] { (void)db.get("key"); });
}

void locking_and_process_exit() {
    Temp temp;
    {
        Store db(temp.path());
        db.put("held", "value");
        expect_error(ErrorCode::locked, [&] { Store second(temp.path()); });
        fs::create_hard_link(temp.path(), temp.path("alias.db"));
        expect_error(ErrorCode::locked, [&] { Store alias(temp.path("alias.db")); });
        const auto child = ::fork();
        if (child == 0) {
            try { Store competing(temp.path()); ::_exit(20); }
            catch (const Error& error) { ::_exit(error.code() == ErrorCode::locked ? 0 : 21); }
            catch (...) { ::_exit(22); }
        }
        child_success(child);
    }
    {
        Store reopened(temp.path());
        require(reopened.get("held") == "value", "lock released after owner closes");
    }
    const auto child = ::fork();
    if (child == 0) {
        try { Store db(temp.path("exit.db")); db.put("persisted", "without-destructor"); ::_exit(0); }
        catch (...) { ::_exit(23); }
    }
    child_success(child);
    Store after_exit(temp.path("exit.db"), {.create_if_missing=false});
    require(after_exit.get("persisted") == "without-destructor", "acknowledged put survives process exit without destructor");
}

void inherited_store_is_rejected() {
    Temp temp;
    auto db = std::make_unique<Store>(temp.path());
    db->put("held", "parent value");
    const auto original = read(temp.path());
    std::cout.flush();
    const auto child = ::fork();
    if (child == 0) {
        try {
            // A fork inherits the same locked open-file description, but an
            // independent index. Reject even an otherwise valid child append.
            expect_error(ErrorCode::invalid_argument, [&] { db->put("child", "rejected"); });
            expect_error(ErrorCode::invalid_argument, [&] { (void)db->erase("held"); });
            expect_error(ErrorCode::invalid_argument, [&] { (void)db->erase("missing"); });
            expect_error(ErrorCode::invalid_argument, [&] { (void)db->get("held"); });
            expect_error(ErrorCode::invalid_argument, [&] { (void)db->get("missing"); });
            expect_error(ErrorCode::invalid_argument, [&] { (void)db->keys(); });
            expect_error(ErrorCode::invalid_argument, [&] { (void)db->keys_page(""); });
            expect_error(ErrorCode::invalid_argument, [&] { (void)db->stats(); });
            require(db->repaired_bytes() == 0, "Historical repair metadata remains readable");
            db.reset(); // Closing the child's descriptor must preserve the parent's lock.
            expect_error(ErrorCode::locked, [&] { Store competing(temp.path()); });
            Store owned(temp.path("child.db"));
            owned.put("fresh", "child-owned");
            require(owned.get("fresh") == "child-owned", "Fresh child Store remains usable");
            ::_exit(0);
        } catch (const std::exception& error) {
            std::cerr << "Inherited Store check failed: " << error.what() << '\n';
            ::_exit(24);
        }
    }
    child_success(child);
    require(read(temp.path()) == original, "Rejected inherited operations preserve database bytes");
    require(db->get("held") == "parent value" && !db->get("child"), "Parent index remains unchanged");
    db->put("after", "parent continues");
    require(db->stats().record_count == 2, "Parent remains writable after child closes its copy");
    db.reset();
    Store reopened(temp.path());
    require(reopened.get("held") == "parent value" && reopened.get("after") == "parent continues",
            "Parent writes survive reopening after fork rejection");
}

struct Pipe {
    std::array<int, 2> descriptors{-1, -1};
    Pipe() {
        if (::pipe(descriptors.data()) != 0) throw std::runtime_error("pipe failed");
    }
    ~Pipe() { close_read(); close_write(); }
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    void close_read() { if (descriptors[0] >= 0) { ::close(descriptors[0]); descriptors[0] = -1; } }
    void close_write() { if (descriptors[1] >= 0) { ::close(descriptors[1]); descriptors[1] = -1; } }
};

void simultaneous_first_creation() {
    constexpr std::size_t participants = 8;
    for (unsigned iteration = 0; iteration != 20; ++iteration) {
        Temp temp;
        Pipe ready;
        Pipe gate;
        std::vector<pid_t> children;
        for (std::size_t index = 0; index != participants; ++index) {
            const auto child = ::fork();
            if (child < 0) break;
            if (child == 0) {
                ready.close_read();
                gate.close_write();
                const char token = 'r';
                ssize_t result;
                do { result = ::write(ready.descriptors[1], &token, 1); } while (result < 0 && errno == EINTR);
                if (result != 1) ::_exit(45);
                ready.close_write();
                char release = 0;
                do { result = ::read(gate.descriptors[0], &release, 1); } while (result < 0 && errno == EINTR);
                if (result != 0) ::_exit(45);
                gate.close_read();
                try {
                    Store db(temp.path());
                    db.put("writer-" + std::to_string(index), "ready");
                    ::_exit(0);
                } catch (const Error& error) {
                    ::_exit(error.code() == ErrorCode::locked ? 42 : 43);
                } catch (...) { ::_exit(44); }
            }
            children.push_back(child);
        }
        ready.close_write();
        gate.close_read();
        std::size_t waiting = 0;
        char token = 0;
        while (waiting < children.size()) {
            const auto result = ::read(ready.descriptors[0], &token, 1);
            if (result < 0 && errno == EINTR) continue;
            if (result != 1) break;
            ++waiting;
        }
        ready.close_read();
        // All children report readiness before this EOF releases them together.
        // Closing also releases/reaps children after fork errors.
        gate.close_write();
        std::vector<int> statuses;
        for (const auto child : children) {
            int status = 0;
            pid_t result;
            do { result = ::waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
            statuses.push_back(result == child && WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        }
        require(children.size() == participants && waiting == participants, "all concurrent creators reached barrier");
        std::size_t successes = 0;
        Store final(temp.path(), {.create_if_missing=false});
        for (std::size_t index = 0; index != statuses.size(); ++index) {
            require(statuses[index] == 0 || statuses[index] == 42, "concurrent creator succeeds or reports locked, never corruption/io");
            const auto value = final.get("writer-" + std::to_string(index));
            if (statuses[index] == 0) {
                ++successes;
                require(value == "ready", "successful concurrent writer persisted its record");
            } else {
                require(!value, "locked concurrent writer did not append");
            }
        }
        require(successes >= 1 && final.stats().key_count == successes && final.stats().record_count == successes, "at least one first creator succeeds and complete history remains valid");
        std::size_t entries = 0;
        for (const auto& entry : fs::directory_iterator(temp.directory)) {
            ++entries;
            require(entry.path() == temp.path(), "successful and colliding creators leave no temporary files");
        }
        require(entries == 1, "creation directory contains only the database");
    }
    Temp temp;
    const auto directory_path = temp.path("directory.db");
    fs::create_directory(directory_path);
    expect_error(ErrorCode::io, [&] { Store rejected(directory_path); });
    std::size_t entries = 0;
    for (const auto& entry : fs::directory_iterator(temp.directory)) {
        ++entries;
        require(entry.path() == directory_path, "rejected directory leaves no temporary creation files");
    }
    require(entries == 1 && fs::is_empty(directory_path), "rejected directory remains unchanged");

    Temp interrupted;
    const auto child = ::fork();
    if (child == 0) {
        struct rlimit limit {};
        if (::getrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(46);
        limit.rlim_cur = 7; // Too short for the mandatory 16-byte file header.
        if (::signal(SIGXFSZ, SIG_IGN) == SIG_ERR || ::setrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(47);
        try { Store incomplete(interrupted.path()); ::_exit(48); }
        catch (const Error& error) { ::_exit(error.code() == ErrorCode::io ? 0 : 49); }
        catch (...) { ::_exit(50); }
    }
    child_success(child);
    require(!fs::exists(interrupted.path()) && fs::is_empty(interrupted.directory), "failed header initialization leaves no database or temporary file");
}

void short_write_poisoning() {
    Temp temp;
    const auto child = ::fork();
    if (child == 0) {
        try {
            Store db(temp.path());
            db.put("before", "safe");
            struct rlimit limit {};
            if (::getrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(30);
            limit.rlim_cur = static_cast<rlim_t>(db.stats().file_bytes + 7);
            if (::signal(SIGXFSZ, SIG_IGN) == SIG_ERR || ::setrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(31);
            try { db.put("broken", "must-not-be-acknowledged"); ::_exit(32); }
            catch (const Error& error) { if (error.code() != ErrorCode::io) ::_exit(33); }
            try { (void)db.get("before"); ::_exit(34); }
            catch (const Error& error) { if (error.code() != ErrorCode::io) ::_exit(35); }
            try { db.put("later", "rejected"); ::_exit(36); }
            catch (const Error& error) { if (error.code() != ErrorCode::io) ::_exit(37); }
            ::_exit(0);
        } catch (...) { ::_exit(38); }
    }
    child_success(child);
    const auto broken = read(temp.path());
    expect_error(ErrorCode::incomplete_tail, [&] { Store db(temp.path(), {.create_if_missing=false}); });
    require(read(temp.path()) == broken, "failed-write tail is preserved until explicit repair");
    Store repaired(temp.path(), {.create_if_missing=false, .repair_incomplete_tail=true});
    require(repaired.repaired_bytes() == 7, "short append discarded precisely");
    require(repaired.get("before") == "safe" && !repaired.get("broken"), "previous acknowledged data survives failed write");
    repaired.put("after", "works");
    require(repaired.get("after") == "works", "repaired file accepts writes");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests{
        {"persistence, updates, deletes and stats", persistence},
        {"binary records and inclusive size limits", binary_and_limits},
        {"CRC and independent on-disk fixture", codec_and_external_fixture},
        {"deterministic operation ledger", map_oracle},
        {"every final-record truncation boundary", partial_tail_recovery},
        {"partial lengths must admit a valid completion", partial_length_validation},
        {"corruption is rejected without mutation", corruption_is_not_repair},
        {"exclusive locking and abrupt process exit", locking_and_process_exit},
        {"inherited Store operations reject forked children", inherited_store_is_rejected},
        {"simultaneous first creation is atomic", simultaneous_first_creation},
        {"short append poisons writer until reopen", short_write_poisoning},
    };
    std::size_t passed = 0;
    for (const auto& [name, test] : tests) {
        try { test(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << passed << '/' << tests.size() << " groups passed; " << assertions << " checks\n";
    return passed == tests.size() ? 0 : 1;
}
