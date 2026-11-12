#include "pocketkv/store.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

// Compile a separate copy of store.cpp with fsync/pread renamed to these two
// wrappers. The production library has no test hooks, and this translation unit
// calls the real system functions after injecting each requested failure.
namespace {

enum class SyncFault { none, io_once, interrupted_once, directory_io_once };
enum class ReadFault { none, io_once, interrupted_once, short_reads };
SyncFault sync_fault = SyncFault::none;
ReadFault read_fault = ReadFault::none;
std::size_t sync_errors = 0;
std::size_t sync_interrupts = 0;
std::size_t read_errors = 0;
std::size_t read_interrupts = 0;
std::size_t shortened_reads = 0;

} // namespace

extern "C" int pocketkv_test_fsync(int fd) {
    if (sync_fault == SyncFault::io_once) {
        sync_fault = SyncFault::none;
        ++sync_errors;
        errno = EIO;
        return -1;
    }
    if (sync_fault == SyncFault::interrupted_once) {
        sync_fault = SyncFault::none;
        ++sync_interrupts;
        errno = EINTR;
        return -1;
    }
    if (sync_fault == SyncFault::directory_io_once) {
        struct stat metadata {};
        if (::fstat(fd, &metadata) == 0 && S_ISDIR(metadata.st_mode)) {
            sync_fault = SyncFault::none;
            ++sync_errors;
            errno = EIO;
            return -1;
        }
    }
    return ::fsync(fd);
}

extern "C" ssize_t pocketkv_test_pread(int fd, void* output, std::size_t length, off_t offset) {
    if (read_fault == ReadFault::io_once) {
        read_fault = ReadFault::none;
        ++read_errors;
        errno = EIO;
        return -1;
    }
    if (read_fault == ReadFault::interrupted_once) {
        read_fault = ReadFault::none;
        ++read_interrupts;
        errno = EINTR;
        return -1;
    }
    if (read_fault == ReadFault::short_reads && length > 3) {
        length = 3;
        ++shortened_reads;
    }
    return ::pread(fd, output, length, offset);
}

namespace {

namespace fs = std::filesystem;
using pocketkv::Error;
using pocketkv::ErrorCode;
using pocketkv::Store;
std::size_t checks = 0;

void require(bool condition, std::string_view description) {
    ++checks;
    if (!condition) throw std::runtime_error(std::string(description));
}

template<class Function>
void expect_io(Function operation) {
    ++checks;
    try {
        operation();
    } catch (const Error& error) {
        if (error.code() == ErrorCode::io) return;
        throw std::runtime_error("Expected I/O error; got: " + std::string(error.what()));
    }
    throw std::runtime_error("Expected I/O error was not thrown");
}

struct ResetFaults {
    ResetFaults() { sync_fault = SyncFault::none; read_fault = ReadFault::none; }
    ~ResetFaults() { sync_fault = SyncFault::none; read_fault = ReadFault::none; }
};

struct TemporaryDirectory {
    fs::path directory;
    TemporaryDirectory() {
        auto pattern = (fs::temp_directory_path() / "pocketkv-fault-XXXXXX").string();
        const char* created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create test directory");
        directory = created;
    }
    ~TemporaryDirectory() { std::error_code ignored; fs::remove_all(directory, ignored); }
    [[nodiscard]] fs::path database() const { return directory / "data.db"; }
};

std::string file_contents(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read test database");
    std::string result{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    if (stream.bad()) throw std::runtime_error("Failed while reading test database");
    return result;
}

void require_poisoned(Store& store) {
    expect_io([&] { (void)store.get("stable"); });
    expect_io([&] { (void)store.keys(); });
    expect_io([&] { (void)store.keys_page(""); });
    expect_io([&] { (void)store.stats(); });
    expect_io([&] { store.put("later", "must be rejected"); });
    expect_io([&] { (void)store.erase("stable"); });
}

void initialization_sync_failure() {
    TemporaryDirectory temp;
    const auto before = sync_errors;
    sync_fault = SyncFault::io_once;
    expect_io([&] { Store store(temp.database()); });
    require(sync_errors == before + 1, "Initialization reached injected sync failure");
    require(!fs::exists(temp.database()) && fs::is_empty(temp.directory),
            "Failed initial sync leaves neither destination nor private temporary file");
    Store retry(temp.database());
    retry.put("stable", "created after failure");
    require(retry.get("stable") == "created after failure", "Creation succeeds after failed initialization");
}

void put_sync_failure() {
    TemporaryDirectory temp;
    {
        Store store(temp.database());
        store.put("stable", "before");
        const auto before = sync_errors;
        sync_fault = SyncFault::io_once;
        expect_io([&] { store.put("stable", "uncertain update"); });
        require(sync_errors == before + 1, "Put reached injected sync failure");
        require_poisoned(store);
    }
    // The wrapper fails fsync after the append completed. The failed mutation
    // must not be acknowledged, but reopening may legitimately find its bytes.
    Store reopened(temp.database(), {.create_if_missing = false});
    require(reopened.get("stable") == "uncertain update", "Reopen sees complete uncertain put");
    require(!reopened.get("later") && reopened.stats().record_count == 2,
            "Poisoned writes never append after failed synchronization");
}

void erase_sync_failure() {
    TemporaryDirectory temp;
    {
        Store store(temp.database());
        store.put("stable", "kept");
        store.put("deleted", "before");
        const auto before = sync_errors;
        sync_fault = SyncFault::io_once;
        expect_io([&] { (void)store.erase("deleted"); });
        require(sync_errors == before + 1, "Erase reached injected sync failure");
        require_poisoned(store);
    }
    Store reopened(temp.database(), {.create_if_missing = false});
    require(reopened.get("stable") == "kept" && !reopened.get("deleted"),
            "Reopen replays complete uncertain deletion and preserves other keys");
    require(!reopened.get("later") && reopened.stats().record_count == 3,
            "Failed synchronization prevents later mutation attempts");
}

void repair_sync_failure() {
    TemporaryDirectory temp;
    std::uint64_t valid_bytes = 0;
    {
        Store store(temp.database());
        store.put("stable", "kept");
        valid_bytes = store.stats().file_bytes;
        store.put("tail", "incomplete");
    }
    fs::resize_file(temp.database(), fs::file_size(temp.database()) - 2);
    const auto before = sync_errors;
    sync_fault = SyncFault::io_once;
    expect_io([&] {
        Store store(temp.database(), {.create_if_missing = false, .repair_incomplete_tail = true});
    });
    require(sync_errors == before + 1, "Repair reached injected sync failure");
    require(fs::file_size(temp.database()) == valid_bytes,
            "A failed repair sync may follow a completed truncation");
    Store reopened(temp.database(), {.create_if_missing = false});
    require(reopened.get("stable") == "kept" && !reopened.get("tail"),
            "Failed repair sync preserves the complete prefix");
    require(reopened.repaired_bytes() == 0 && reopened.stats().record_count == 1,
            "Reopening the truncated prefix requires no additional repair");
}

void directory_sync_failure() {
    TemporaryDirectory temp;
    const auto before = sync_errors;
    sync_fault = SyncFault::directory_io_once;
    expect_io([&] { Store store(temp.database()); });
    require(sync_errors == before + 1, "Creation reached injected directory sync failure");
    std::size_t entries = 0;
    for (const auto& entry : fs::directory_iterator(temp.directory)) {
        ++entries;
        require(entry.path() == temp.database(), "Directory sync failure leaves no private temporary entry");
    }
    require(entries == 1, "Directory sync failure follows publication of one database");
    Store reopened(temp.database(), {.create_if_missing = false});
    require(reopened.stats().file_bytes == 16 && reopened.keys().empty(),
            "Published database remains structurally valid after directory sync failure");
}

void interrupted_and_short_io() {
    TemporaryDirectory temp;
    const std::string value(100, 'v');
    {
        Store store(temp.database());
        const auto before_sync = sync_interrupts;
        sync_fault = SyncFault::interrupted_once;
        store.put("stable", value);
        require(sync_interrupts == before_sync + 1, "Interrupted sync was injected and retried");
        const auto before_read = read_interrupts;
        read_fault = ReadFault::interrupted_once;
        require(store.get("stable") == value, "Read succeeds after EINTR");
        require(read_interrupts == before_read + 1, "Interrupted read was injected");
        const auto before_short = shortened_reads;
        read_fault = ReadFault::short_reads;
        require(store.get("stable") == value, "Lookup assembles a record from short reads");
        require(shortened_reads > before_short, "Lookup actually received shortened reads");
    }
    const auto before_short = shortened_reads;
    Store reopened(temp.database(), {.create_if_missing = false});
    require(reopened.get("stable") == value, "Startup replay and lookup tolerate short reads");
    require(shortened_reads > before_short, "Replay actually received shortened reads");
}

void read_failure_is_nonmutating() {
    TemporaryDirectory temp;
    std::string original;
    {
        Store store(temp.database());
        store.put("stable", "kept");
        original = file_contents(temp.database());
        const auto before = read_errors;
        read_fault = ReadFault::io_once;
        expect_io([&] { (void)store.get("stable"); });
        require(read_errors == before + 1, "Lookup reached injected read failure");
        require(store.get("stable") == "kept", "A read error does not poison an unchanged Store");
        require(file_contents(temp.database()) == original, "Read failure never changes database bytes");
    }
    const auto before = read_errors;
    read_fault = ReadFault::io_once;
    expect_io([&] { Store store(temp.database(), {.create_if_missing = false}); });
    require(read_errors == before + 1, "Startup replay reached injected read failure");
    require(file_contents(temp.database()) == original, "Failed replay never changes database bytes");
    Store reopened(temp.database(), {.create_if_missing = false});
    require(reopened.get("stable") == "kept", "Failed replay releases its descriptor and lock");
}

void snapshot_failures_preserve_source() {
    TemporaryDirectory temp;
    Store source(temp.database());
    source.put("stable", "original");
    const auto original = file_contents(temp.database());
    const auto destination = temp.directory / "snapshot.db";
    sync_fault = SyncFault::io_once;
    expect_io([&] { source.snapshot(destination); });
    require(!fs::exists(destination), "Failed snapshot sync never publishes its destination");
    read_fault = ReadFault::io_once;
    expect_io([&] { source.snapshot(destination); });
    require(!fs::exists(destination), "Failed source read never publishes its destination");
    require(file_contents(temp.database()) == original, "Snapshot failures preserve source bytes");
    require(source.get("stable") == "original", "Snapshot failure never poisons the source");
    sync_fault = SyncFault::directory_io_once;
    expect_io([&] { source.snapshot(destination); });
    { Store copy(destination); require(copy.get("stable") == "original", "Failed directory sync leaves complete published snapshot"); }
    for (const auto& entry : fs::directory_iterator(temp.directory)) {
        require(entry.path() == temp.database() || entry.path() == destination, "Snapshot failure leaves no temporary file");
    }
}

} // namespace

int main() {
    const std::vector<std::pair<std::string_view, std::function<void()>>> groups{
        {"initial file sync failure cleans up creation", initialization_sync_failure},
        {"failed put sync poisons the Store", put_sync_failure},
        {"failed erase sync poisons the Store", erase_sync_failure},
        {"failed repair sync preserves the valid prefix", repair_sync_failure},
        {"directory sync failure leaves a valid published file", directory_sync_failure},
        {"interrupted synchronization and short reads", interrupted_and_short_io},
        {"read failures preserve bytes and release failed opens", read_failure_is_nonmutating},
        {"snapshot failures preserve source and clean private files", snapshot_failures_preserve_source},
    };
    std::size_t passed = 0;
    for (const auto& [name, test] : groups) {
        const ResetFaults reset;
        try {
            test();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << passed << '/' << groups.size() << " fault groups passed; " << checks << " checks\n";
    return passed == groups.size() ? 0 : 1;
}
