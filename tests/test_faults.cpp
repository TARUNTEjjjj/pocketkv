#include "pocketkv/store.hpp"
#include "../src/format.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <utility>
#include <vector>

// Compile a separate copy of store.cpp with fsync, pread, mkostemp, writev, fstat
// and geteuid (and, on Apple platforms, fcntl) renamed to these wrappers. The
// production library has no test hooks, and this translation unit calls the
// real system functions after injecting each requested failure.
namespace {

enum class SyncFault { none, io_once, interrupted_once, directory_io_once, directory_interrupted_once };
enum class ReadFault { none, io_once, interrupted_once, short_reads };
enum class WriteFault { none, interrupted_once, short_writes };
SyncFault sync_fault = SyncFault::none;
ReadFault read_fault = ReadFault::none;
WriteFault write_fault = WriteFault::none;
std::size_t write_limit = 0;           // short_writes: bytes the system takes per call
std::size_t record_writes = 0;         // writev calls that reached the system
std::size_t write_interrupts = 0;
std::optional<uid_t> reported_owner;   // fstat reports this owner for regular files
std::optional<uid_t> reported_caller;  // geteuid reports this effective user
std::filesystem::path competing_database; // Published by a rival creator during mkostemp.
bool competing_published = false;
std::size_t sync_errors = 0;
std::size_t sync_interrupts = 0;
std::size_t plain_syncs = 0;     // fsync calls that reached the system
std::size_t directory_syncs = 0; // syncs of either kind applied to a directory
#ifdef F_FULLFSYNC
bool full_sync_unsupported = false; // The filesystem rejects F_FULLFSYNC.
std::size_t full_syncs = 0;         // F_FULLFSYNC calls that reached the system
#endif
std::size_t read_errors = 0;
std::size_t read_interrupts = 0;
std::size_t shortened_reads = 0;
std::size_t private_files = 0;
std::size_t inheritable_private_files = 0;

bool is_directory(int fd) {
    struct stat metadata {};
    return ::fstat(fd, &metadata) == 0 && S_ISDIR(metadata.st_mode);
}

// Apply the armed fault to whichever call makes data durable. Returns true,
// with errno set, when this call must fail.
bool injected_sync_failure(int fd) {
    const bool directory_only = sync_fault == SyncFault::directory_io_once ||
                                sync_fault == SyncFault::directory_interrupted_once;
    if (sync_fault == SyncFault::none || (directory_only && !is_directory(fd))) return false;
    const bool interrupt = sync_fault == SyncFault::interrupted_once ||
                           sync_fault == SyncFault::directory_interrupted_once;
    sync_fault = SyncFault::none;
    if (interrupt) {
        ++sync_interrupts;
        errno = EINTR;
    } else {
        ++sync_errors;
        errno = EIO;
    }
    return true;
}

void count_sync(int fd, std::size_t& primitive) {
    ++primitive;
    if (is_directory(fd)) ++directory_syncs;
}

} // namespace

extern "C" int pocketkv_test_fsync(int fd) {
    if (injected_sync_failure(fd)) return -1;
    count_sync(fd, plain_syncs);
    return ::fsync(fd);
}

#ifdef F_FULLFSYNC
extern "C" int pocketkv_test_fcntl(int fd, int operation, int value) {
    if (operation != F_FULLFSYNC) return ::fcntl(fd, operation, value);
    if (full_sync_unsupported) {
        errno = ENOTSUP;
        return -1;
    }
    if (injected_sync_failure(fd)) return -1;
    count_sync(fd, full_syncs);
    return ::fcntl(fd, F_FULLFSYNC);
}
#endif

extern "C" int pocketkv_test_mkostemp(char* name, int flags) {
    const int fd = ::mkostemp(name, flags);
    if (fd < 0) return fd;
    ++private_files;
    // Another thread could fork and exec at this very moment.
    if ((::fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0) ++inheritable_private_files;
    if (!competing_database.empty()) {
        // A rival creator publishes its header-only file first, then dies
        // before it can sync the directory.
        const auto header = pocketkv::detail::make_file_header();
        std::ofstream output(competing_database, std::ios::binary);
        output.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
        competing_published = static_cast<bool>(output.flush());
        competing_database.clear();
    }
    return fd;
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

extern "C" ssize_t pocketkv_test_writev(int fd, const struct iovec* parts, int count) {
    if (write_fault == WriteFault::interrupted_once) {
        write_fault = WriteFault::none;
        ++write_interrupts;
        errno = EINTR;
        return -1;
    }
    ++record_writes;
    if (write_fault != WriteFault::short_writes) return ::writev(fd, parts, count);
    // Let the system take only a prefix, as a nearly full disk can.
    std::array<iovec, 8> prefix {};
    std::size_t used = 0;
    std::size_t remaining = write_limit;
    for (int index = 0; index < count && used < prefix.size() && remaining != 0; ++index) {
        const auto length = std::min(parts[index].iov_len, remaining);
        prefix[used++] = iovec{parts[index].iov_base, length};
        remaining -= length;
    }
    return ::writev(fd, prefix.data(), static_cast<int>(used));
}

extern "C" int pocketkv_test_fstat(int fd, struct stat* metadata) {
    const int status = ::fstat(fd, metadata);
    if (status == 0 && reported_owner && S_ISREG(metadata->st_mode)) metadata->st_uid = *reported_owner;
    return status;
}

extern "C" uid_t pocketkv_test_geteuid() { return reported_caller ? *reported_caller : ::geteuid(); }

// AddressSanitizer brings its own operator new, so only ordinary builds replace
// it to count allocations and fail one on request.
#if defined(__SANITIZE_ADDRESS__)
#define POCKETKV_TEST_ALLOCATOR 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define POCKETKV_TEST_ALLOCATOR 0
#endif
#endif
#ifndef POCKETKV_TEST_ALLOCATOR
#define POCKETKV_TEST_ALLOCATOR 1
#endif

#if POCKETKV_TEST_ALLOCATOR
namespace {
std::size_t allocations = 0;        // global operator new calls
std::size_t failing_allocation = 0; // 0: none; n: the n-th allocation from now throws
} // namespace

void* operator new(std::size_t size) {
    ++allocations;
    if (failing_allocation != 0 && --failing_allocation == 0) throw std::bad_alloc();
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
#endif

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
    ResetFaults() { reset(); }
    ~ResetFaults() { reset(); }
    static void reset() {
        sync_fault = SyncFault::none;
        read_fault = ReadFault::none;
        write_fault = WriteFault::none;
        reported_owner.reset();
        reported_caller.reset();
#ifdef F_FULLFSYNC
        full_sync_unsupported = false;
#endif
#if POCKETKV_TEST_ALLOCATOR
        failing_allocation = 0;
#endif
        competing_database.clear();
        competing_published = false;
    }
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

// The call that makes data durable: F_FULLFSYNC on Apple platforms, where
// fsync() can leave data in the drive's volatile write cache; else fsync().
std::size_t durable_syncs() {
#ifdef F_FULLFSYNC
    return full_syncs;
#else
    return plain_syncs;
#endif
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

// The wrapper fails the sync after the append completed. The record must not
// survive into the next open: Linux can keep pages whose writeback failed in
// its cache, readable although they never reached the disk, and records
// acknowledged after them would be lost with them. The writer therefore cuts
// the file back to its acknowledged bytes and syncs that.
void put_sync_failure() {
    TemporaryDirectory temp;
    std::string acknowledged;
    {
        Store store(temp.database());
        store.put("stable", "before");
        acknowledged = file_contents(temp.database());
        const auto before = sync_errors;
        const auto syncs = durable_syncs();
        sync_fault = SyncFault::io_once;
        expect_io([&] { store.put("stable", "uncertain update"); });
        require(sync_errors == before + 1, "Put reached injected sync failure");
        require(file_contents(temp.database()) == acknowledged,
                "A put whose sync failed is cut back to the acknowledged bytes");
        require(durable_syncs() == syncs + 1, "The cut is synced");
        require_poisoned(store);
    }
    Store reopened(temp.database(), {.create_if_missing = false});
    require(reopened.get("stable") == "before", "Reopen sees only the acknowledged value");
    require(!reopened.get("later") && reopened.stats().record_count == 1,
            "Poisoned writes never append after failed synchronization");
    reopened.put("next", "acknowledged");
    require(reopened.stats().file_bytes == acknowledged.size() + 24 + 4 + 12,
            "The next acknowledged record directly follows the last acknowledged one");
}

void erase_sync_failure() {
    TemporaryDirectory temp;
    std::string acknowledged;
    {
        Store store(temp.database());
        store.put("stable", "kept");
        store.put("deleted", "before");
        acknowledged = file_contents(temp.database());
        const auto before = sync_errors;
        sync_fault = SyncFault::io_once;
        expect_io([&] { (void)store.erase("deleted"); });
        require(sync_errors == before + 1, "Erase reached injected sync failure");
        require(file_contents(temp.database()) == acknowledged,
                "An erase whose sync failed is cut back to the acknowledged bytes");
        require_poisoned(store);
    }
    Store reopened(temp.database(), {.create_if_missing = false});
    require(reopened.get("stable") == "kept" && reopened.get("deleted") == "before",
            "Reopen keeps every key whose deletion was not acknowledged");
    require(!reopened.get("later") && reopened.stats().record_count == 2,
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
    // The failed creator never synced the directory, so the next open must.
    const auto directories = directory_syncs;
    Store reopened(temp.database(), {.create_if_missing = false});
    require(directory_syncs == directories + 1,
            "Reopening a database whose directory sync failed syncs the directory");
    require(reopened.stats().file_bytes == 16 && reopened.keys().empty(),
            "Published database remains structurally valid after directory sync failure");
}

void interrupted_and_short_io() {
    TemporaryDirectory temp;
    const std::string value(100, 'v');
    {
        const auto before_directory = sync_interrupts;
        const auto directories = directory_syncs;
        sync_fault = SyncFault::directory_interrupted_once;
        Store store(temp.database());
        require(sync_interrupts == before_directory + 1 && directories + 1 == directory_syncs,
                "Interrupted directory sync was injected and retried");
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

void commits_use_durable_sync() {
    TemporaryDirectory temp;
    const auto plain = plain_syncs;
    const auto directories = directory_syncs;
    auto before = durable_syncs();
    {
        Store store(temp.database());
        require(durable_syncs() == before + 2 && directory_syncs == directories + 1,
                "Creation durably syncs its header and its directory");
        before = durable_syncs();
        store.put("stable", "kept");
        store.put("tail", "erased");
        require(durable_syncs() == before + 2, "Each put durably syncs its record");
        before = durable_syncs();
        require(store.erase("tail"), "Existing key is erased");
        require(durable_syncs() == before + 1, "Erase durably syncs its tombstone");
        before = durable_syncs();
        const auto published = directory_syncs;
        store.snapshot(temp.directory / "snapshot.db");
        require(durable_syncs() == before + 2 && directory_syncs == published + 1,
                "A snapshot durably syncs its file and its directory");
    }
    fs::resize_file(temp.database(), fs::file_size(temp.database()) - 2);
    before = durable_syncs();
    {
        Store repaired(temp.database(), {.create_if_missing = false, .repair_incomplete_tail = true});
        require(repaired.repaired_bytes() == 24 + 4 - 2 && durable_syncs() == before + 1,
                "Repair durably syncs its truncation");
    }
#ifdef F_FULLFSYNC
    require(plain_syncs == plain, "Where F_FULLFSYNC works, no commit settles for fsync()");
#else
    (void)plain;
#endif
}

#ifdef F_FULLFSYNC
// A filesystem without F_FULLFSYNC rejects the request itself; fall back to
// fsync() there. A real F_FULLFSYNC failure must never be retried as fsync().
void full_sync_fallback() {
    TemporaryDirectory temp;
    {
        full_sync_unsupported = true;
        const auto plain = plain_syncs;
        const auto full = full_syncs;
        Store store(temp.database());
        store.put("stable", "kept");
        require(plain_syncs == plain + 3 && full_syncs == full,
                "Unsupported F_FULLFSYNC falls back to fsync() for header, directory and put");
        store.snapshot(temp.directory / "snapshot.db");
        require(plain_syncs == plain + 5 && full_syncs == full,
                "Unsupported F_FULLFSYNC falls back to fsync() for a snapshot's file and directory");
        const auto before = sync_errors;
        sync_fault = SyncFault::io_once;
        expect_io([&] { store.put("stable", "uncertain update"); });
        require(sync_errors == before + 1, "A failed fallback fsync() is reported");
        require_poisoned(store);
        full_sync_unsupported = false;
    }
    Store reopened(temp.database(), {.create_if_missing = false});
    const auto plain = plain_syncs;
    const auto before = sync_errors;
    sync_fault = SyncFault::io_once;
    expect_io([&] { reopened.put("stable", "uncertain again"); });
    require(sync_errors == before + 1 && plain_syncs == plain,
            "An F_FULLFSYNC I/O error is reported, never retried as fsync()");
    require_poisoned(reopened);
}
#endif

// A new database's private file must be close-on-exec from the moment it
// exists. Setting FD_CLOEXEC afterwards leaves a window in which another
// thread's fork/exec hands the descriptor, and later its lock, to a program.
void private_file_close_on_exec() {
    TemporaryDirectory temp;
    const auto files = private_files;
    const auto inheritable = inheritable_private_files;
    {
        Store store(temp.database());
        store.put("stable", "kept");
    }
    require(private_files == files + 1, "Creation makes its private file with mkostemp");
    require(inheritable_private_files == inheritable, "The private file is close-on-exec from creation");
    {
        Store store(temp.database(), {.create_if_missing = false});
        store.snapshot(temp.directory / "snapshot.db");
    }
    require(private_files == files + 2, "A snapshot makes its private file with mkostemp");
    require(inheritable_private_files == inheritable, "A snapshot's private file is close-on-exec from creation");
}

// A creator publishes a header-only file before it syncs the directory. A Store
// that adopts such a file, rather than creating it, must sync the directory
// itself: the creator may have failed or died before doing so.
void adopted_database_syncs_directory() {
    TemporaryDirectory raced;
    competing_database = raced.database();
    auto before = directory_syncs;
    {
        Store loser(raced.database());
        require(competing_published, "A rival creator published the database first");
        require(directory_syncs == before + 1, "A creator that loses the race syncs the directory it adopts");
        loser.put("after", "race");
    }
    std::size_t entries = 0;
    for (const auto& entry : fs::directory_iterator(raced.directory)) {
        ++entries;
        require(entry.path() == raced.database(), "The losing creator leaves no private file");
    }
    require(entries == 1, "Only the adopted database remains");
    before = directory_syncs;
    Store reopened(raced.database(), {.create_if_missing = false});
    require(reopened.get("after") == "race", "The adopted database keeps acknowledged records");
    require(directory_syncs == before, "A database that already has records needs no directory sync");
}

// A record reaches the file in one writev() call, so a signal that kills the
// process cannot fall between its header and its payload. Interrupted and short
// writes resume exactly where they stopped, inside the header, key or value.
void records_use_one_write() {
    TemporaryDirectory temp;
    const std::string value = "value";
    const auto record = pocketkv::detail::record_header_size + 1 + value.size();
    {
        Store store(temp.database());
        auto before = record_writes;
        store.put("stable", "kept");
        require(record_writes == before + 1, "A put hands its header, key and value to one writev()");
        before = record_writes;
        store.put("empty", "");
        require(record_writes == before + 1, "A put of an empty value makes one writev()");
        before = record_writes;
        require(store.erase("empty"), "Existing key is erased");
        require(record_writes == before + 1, "An erase makes one writev()");
        const auto interrupts = write_interrupts;
        write_fault = WriteFault::interrupted_once;
        store.put("interrupted", value);
        require(write_interrupts == interrupts + 1 && store.get("interrupted") == value,
                "An interrupted writev() is retried");
        for (std::size_t limit = 1; limit < record; ++limit) {
            write_fault = WriteFault::short_writes;
            write_limit = limit;
            before = record_writes;
            store.put("k", value);
            write_fault = WriteFault::none;
            require(record_writes == before + (record + limit - 1) / limit,
                    "Each short write resumes after the bytes already written");
            require(store.get("k") == value, "A record assembled from short writes reads back");
        }
        before = record_writes;
        store.snapshot(temp.directory / "snapshot.db");
        require(record_writes == before + 1 + store.stats().key_count,
                "A snapshot makes one writev() for its file header and one for each record");
    }
    Store reopened(temp.database(), {.create_if_missing = false});
    require(reopened.get("stable") == "kept" && reopened.get("k") == value && !reopened.get("empty"),
            "Records written in pieces replay");
    require(reopened.stats().record_count == 4 + (record - 1), "Every record written in pieces is complete");
}

// Another user can plant a valid database where a victim will open it, such as
// /tmp/app.db. In a sticky directory that other users can write, a Store must
// refuse an existing database unless the caller or the directory's owner owns
// it, as Linux's fs.protected_regular=2 does for O_CREAT opens. The wrappers
// report a foreign owner or caller, since a test cannot create another user.
void foreign_database_in_shared_directory() {
    TemporaryDirectory temp;
    const auto shared = temp.directory / "shared";
    fs::create_directory(shared);
    const auto database = shared / "app.db";
    {
        Store owner(database);
        owner.put("planted", "value");
    }
    const auto planted = file_contents(database);
    const uid_t other = ::geteuid() == 4242U ? 4243U : 4242U;
    const auto opens = [&](bool create_if_missing) {
        try {
            Store opened(database, {.create_if_missing = create_if_missing});
            return opened.get("planted") == "value";
        } catch (const Error& error) {
            if (error.code() == ErrorCode::invalid_argument) return false;
            throw;
        }
    };
    using fs::perms;
    for (const auto mode : {perms::all | perms::sticky_bit, perms::owner_all | perms::group_all | perms::sticky_bit}) {
        fs::permissions(shared, mode);
        reported_owner = other;
        require(!opens(false) && !opens(true), "A foreign database in a shared sticky directory is refused");
        reported_owner.reset();
        require(file_contents(database) == planted, "A refused database is not changed");
        require(opens(false), "A refused database keeps no lock");
        reported_caller = other;
        require(opens(false), "A database owned by the shared directory's owner is trusted");
        reported_caller.reset();
    }
    for (const auto mode : {perms::owner_all | perms::sticky_bit, perms::all}) {
        fs::permissions(shared, mode);
        reported_owner = other;
        require(opens(false), "Ownership is not checked where no other user may add files, or anyone may replace them");
        reported_owner.reset();
    }
    // A creator that loses the race applies the same rule to the rival's file.
    fs::permissions(shared, perms::all | perms::sticky_bit);
    const auto raced = shared / "raced.db";
    competing_database = raced;
    reported_owner = other;
    bool refused = false;
    try {
        Store loser(raced);
    } catch (const Error& error) {
        refused = error.code() == ErrorCode::invalid_argument;
    }
    reported_owner.reset();
    require(competing_published && refused, "A losing creator refuses a foreign rival database");
    require(fs::file_size(raced) == pocketkv::detail::file_header_size,
            "The refused rival database is not changed");
    std::size_t entries = 0;
    for (const auto& entry : fs::directory_iterator(shared)) {
        ++entries;
        require(entry.path() == database || entry.path() == raced, "The losing creator leaves no private file");
    }
    require(entries == 2, "Only the two databases remain");
    fs::permissions(shared, perms::owner_all);
}

#if POCKETKV_TEST_ALLOCATOR
// A put allocates what the index needs before it appends. Running out of memory
// must fail the put before anything is written, never after its sync, where a
// committed record would be reported as an error.
void allocation_failure_precedes_append() {
    TemporaryDirectory temp;
    Store store(temp.database());
    store.put("stable", "kept");
    for (const auto& key : {std::string("k"), std::string(40, 'k')}) {
        std::size_t failures = 0;
        for (std::size_t failing = 1; failing <= 8; ++failing) {
            const auto before = file_contents(temp.database());
            bool failed = false;
            failing_allocation = failing;
            try {
                store.put(key, "value");
            } catch (const std::bad_alloc&) {
                failed = true;
            }
            failing_allocation = 0;
            if (!failed) break;
            ++failures;
            require(file_contents(temp.database()) == before, "An allocation failure in put writes nothing");
            require(store.get("stable") == "kept" && !store.get(key),
                    "After an allocation failure the Store stays usable and the key absent");
        }
        require(failures != 0, "Putting a new key allocates before appending");
        require(store.get(key) == "value", "The put succeeds once memory is available");
    }
}

// Replay reuses one payload buffer and copies a key only when the key enters
// the index, so the allocations of an open follow live keys, not history.
void replay_allocations_follow_live_keys() {
    TemporaryDirectory temp;
    const std::string key(40, 'k'); // Longer than any small-string buffer.
    const std::string value(64, 'v');
    const auto opening_allocations = [&] {
        const auto path = temp.database();
        const auto before = allocations;
        Store reopened(path, {.create_if_missing = false});
        const auto used = allocations - before;
        require(reopened.get(key) == value && reopened.stats().key_count == 1, "Replay finds the latest value");
        return used;
    };
    {
        Store store(temp.database());
        for (int round = 0; round != 2; ++round) store.put(key, value);
    }
    const auto short_history = opening_allocations();
    {
        Store store(temp.database(), {.create_if_missing = false});
        for (int round = 0; round != 16; ++round) store.put(key, value);
    }
    const auto long_history = opening_allocations();
    require(long_history == short_history,
            "Opening 18 records of one key allocates as much as opening 2 (" +
            std::to_string(long_history) + " against " + std::to_string(short_history) + ")");
}
#endif

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
        {"every commit point reaches the durable sync", commits_use_durable_sync},
#ifdef F_FULLFSYNC
        {"unsupported F_FULLFSYNC falls back to fsync; failures do not", full_sync_fallback},
#endif
        {"new private files are close-on-exec from creation", private_file_close_on_exec},
        {"adopting an unsynced database syncs its directory", adopted_database_syncs_directory},
        {"each record reaches the file in one writev", records_use_one_write},
        {"a foreign database in a shared sticky directory is refused", foreign_database_in_shared_directory},
#if POCKETKV_TEST_ALLOCATOR
        {"an allocation failure in put precedes its append", allocation_failure_precedes_append},
        {"replay allocations follow live keys, not history", replay_allocations_follow_live_keys},
#endif
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
