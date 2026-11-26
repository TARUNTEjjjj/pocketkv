#include "pocketkv/store.hpp"
#include "format.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <utility>

namespace pocketkv {
namespace {

class FileDescriptor {
public:
    explicit FileDescriptor(int value = -1) noexcept : value_(value) {}
    ~FileDescriptor() { reset(); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept
        : value_(std::exchange(other.value_, -1)), lock_owner_(std::exchange(other.lock_owner_, 0)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, -1);
            lock_owner_ = std::exchange(other.lock_owner_, 0);
        }
        return *this;
    }
    [[nodiscard]] int get() const noexcept { return value_; }
    // Record that this process now holds the descriptor's flock.
    void mark_locked() noexcept { lock_owner_ = ::getpid(); }
private:
    void reset() noexcept {
        if (value_ < 0) return;
        // A flock belongs to the open file description, which a child forked
        // while the lock is held shares. Unlock explicitly, so the lock ends
        // when the process that took it closes the descriptor rather than when
        // the last forked copy does. A child closing its inherited copy must
        // not unlock: that would release the parent's lock.
        if (lock_owner_ != 0 && lock_owner_ == ::getpid()) (void)::flock(value_, LOCK_UN);
        ::close(value_);
        value_ = -1;
        lock_owner_ = 0;
    }
    int value_;
    pid_t lock_owner_ = 0; // getpid() of the locking process; 0 when unlocked
};

[[noreturn]] void io_error(std::string_view operation, int code = errno) {
    throw Error(ErrorCode::io, std::string(operation) + ": " + std::strerror(code));
}

// A process may start with stdin, stdout or stderr closed (a shell's `>&-`).
// open() and mkostemp() return the lowest free descriptor, so a database could
// take one of those slots and receive the program's own console output. Move
// such a descriptor above 2 and close the original, leaving the slot closed so
// those writes keep failing. Returns -1 with errno set if the move fails.
int above_standard_streams(int fd) {
    if (fd < 0 || fd > STDERR_FILENO) return fd;
    int moved;
    do {
        moved = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    } while (moved < 0 && errno == EINTR);
    const int failure = errno;
    ::close(fd);
    errno = failure;
    return moved;
}

int open_file(const std::filesystem::path& path, int flags, mode_t mode = 0600) {
    int result;
    do {
        result = ::open(path.c_str(), flags, mode);
    } while (result < 0 && errno == EINTR);
    return above_standard_streams(result);
}

std::filesystem::path parent_directory(const std::filesystem::path& path) {
    auto parent = path.parent_path();
    if (parent.empty()) parent = ".";
    return parent;
}

class TemporaryFile {
public:
    explicit TemporaryFile(const std::filesystem::path& directory)
        : name_((directory / ".pocketkv-XXXXXX").string()) {
        // mkostemp creates a private 0600 file with O_EXCL. O_CLOEXEC applies
        // atomically at creation, so another thread's fork/exec can never
        // inherit the descriptor. All potentially allocating work precedes
        // creation so no exception can lose its name.
        const int created = ::mkostemp(name_.data(), O_CLOEXEC);
        if (created < 0) io_error("cannot create private database file");
        const int raw = above_standard_streams(created);
        if (raw < 0) {
            // The destructor will not run for a throwing constructor.
            const int failure = errno;
            remove_name();
            io_error("cannot create private database file", failure);
        }
        fd_ = FileDescriptor(raw);
        owns_name_ = true;
    }
    ~TemporaryFile() {
        if (owns_name_) (void)unlink_name();
    }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    [[nodiscard]] int get() const noexcept { return fd_.get(); }
    [[nodiscard]] FileDescriptor& descriptor() noexcept { return fd_; }
    [[nodiscard]] const char* name() const noexcept { return name_.c_str(); }
    [[nodiscard]] FileDescriptor release_descriptor() noexcept { return std::move(fd_); }
    void remove_name() {
        if (unlink_name() < 0) io_error("cannot remove private database file");
        owns_name_ = false;
    }
private:
    int unlink_name() noexcept {
        int status;
        do { status = ::unlink(name_.c_str()); } while (status < 0 && errno == EINTR);
        return status;
    }
    std::string name_;
    FileDescriptor fd_;
    bool owns_name_ = false;
};

int control_file(int fd, int operation, int value = 0) {
    int result;
    do { result = ::fcntl(fd, operation, value); } while (result < 0 && errno == EINTR);
    if (result < 0) io_error("cannot configure database descriptor");
    return result;
}

struct stat inspect_file(int fd) {
    struct stat result {};
    int status;
    do {
        status = ::fstat(fd, &result);
    } while (status < 0 && errno == EINTR);
    if (status < 0) io_error("cannot inspect database");
    if (!S_ISREG(result.st_mode)) {
        throw Error(ErrorCode::invalid_argument, "database must be a regular file");
    }
    if (result.st_size < 0) {
        throw Error(ErrorCode::io, "database has an invalid file size");
    }
    return result;
}

void lock_file(FileDescriptor& fd) {
    (void)inspect_file(fd.get()); // Reject FIFOs/devices before attempting a lock.
    int status;
    do { status = ::flock(fd.get(), LOCK_EX | LOCK_NB); } while (status < 0 && errno == EINTR);
    if (status < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            throw Error(ErrorCode::locked, "database is already open; PocketKV requires an exclusive lock");
        }
        io_error("cannot lock database");
    }
    fd.mark_locked();
}

#ifdef F_FULLFSYNC
// Errors with which a filesystem rejects F_FULLFSYNC itself (some network,
// FUSE and device-backed mounts), as opposed to failing to store data.
bool full_sync_unsupported(int code) noexcept {
    return code == ENOTSUP || code == EOPNOTSUPP || code == ENOTTY || code == EINVAL || code == ENODEV;
}
#endif

// Make a file's or directory's contents durable. Apple's fsync() only hands
// the data to the drive, whose volatile write cache can still lose or reorder
// it on power loss; F_FULLFSYNC also flushes that cache. Fall back to fsync()
// only where the filesystem does not support F_FULLFSYNC, never after a real
// failure such as EIO. Linux fsync() already flushes the drive's cache.
void sync_file(int fd, std::string_view failure = "cannot synchronize database") {
    int status;
#ifdef F_FULLFSYNC
    do {
        status = ::fcntl(fd, F_FULLFSYNC);
    } while (status < 0 && errno == EINTR);
    if (status == 0) return;
    if (!full_sync_unsupported(errno)) io_error(failure);
#endif
    do {
        status = ::fsync(fd);
    } while (status < 0 && errno == EINTR);
    if (status < 0) io_error(failure);
}

void sync_parent(const std::filesystem::path& path) {
    FileDescriptor directory(open_file(parent_directory(path), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.get() < 0) io_error("cannot open database directory");
    sync_file(directory.get(), "cannot synchronize database directory");
}

// A Store needs an open file description of its own: O_APPEND puts every
// record at the end of the file, and flock() excludes every other description.
// macOS resolves /dev/fd/N to a duplicate of the caller's descriptor N instead,
// without our O_APPEND and sharing that description's lock, so appends would
// overwrite the log at the caller's offset. Its path reports devfs metadata,
// unlike the file it opened. (Linux open() always creates a new description,
// and its /dev/fd entries are symlinks, which O_NOFOLLOW refuses.) Check before
// locking, so a rejected alias never locks, or later unlocks, the caller's
// description.
void require_own_description(int fd, [[maybe_unused]] const std::filesystem::path& path) {
    bool own = (control_file(fd, F_GETFL) & O_APPEND) != 0;
#if defined(__APPLE__)
    if (own) {
        const auto opened = inspect_file(fd);
        struct stat named {};
        int status;
        do {
            status = ::lstat(path.c_str(), &named);
        } while (status < 0 && errno == EINTR);
        if (status < 0) io_error("cannot inspect database path");
        own = named.st_dev == opened.st_dev && named.st_ino == opened.st_ino;
    }
#endif
    if (!own) {
        throw Error(ErrorCode::invalid_argument,
                    "database path must name the file itself, not an open descriptor such as /dev/fd/N");
    }
}

// Another local user can plant a valid database where a victim will open it,
// such as /tmp/app.db, then read the victim's records or forge values. Like
// Linux's fs.protected_regular=2 for O_CREAT opens, refuse an existing file in a
// sticky directory that other users can write (world- or group-writable) unless
// the caller or the directory's owner owns it. Elsewhere, whoever can write the
// directory can replace its files anyway.
void require_trusted_owner(int fd, const std::filesystem::path& path) {
    const auto owner = inspect_file(fd).st_uid;
    if (owner == ::geteuid()) return;
    struct stat directory {};
    int status;
    do {
        status = ::stat(parent_directory(path).c_str(), &directory);
    } while (status < 0 && errno == EINTR);
    if (status < 0) io_error("cannot inspect database directory");
    const bool shared = (directory.st_mode & S_ISVTX) != 0 && (directory.st_mode & (S_IWGRP | S_IWOTH)) != 0;
    if (shared && owner != directory.st_uid) {
        throw Error(ErrorCode::invalid_argument,
                    "database in a shared sticky directory belongs to another user");
    }
}

// Write every byte of parts, resuming after interruptions and short writes.
// One writev() call normally takes a whole record, so a signal that kills the
// process cannot fall between separate writes of its header and payload.
void write_all(int fd, std::span<iovec> parts) {
    std::size_t next = 0; // The first part with bytes left to write.
    while (true) {
        while (next < parts.size() && parts[next].iov_len == 0) ++next;
        if (next == parts.size()) return;
        const auto result = ::writev(fd, parts.data() + next, static_cast<int>(parts.size() - next));
        if (result < 0) {
            if (errno == EINTR) continue;
            io_error("cannot append database record");
        }
        if (result == 0) io_error("database append made no progress", EIO);
        for (auto done = static_cast<std::size_t>(result); done != 0 && next < parts.size();) {
            auto& part = parts[next];
            const auto step = std::min(done, part.iov_len);
            part.iov_base = static_cast<char*>(part.iov_base) + step;
            part.iov_len -= step;
            done -= step;
            if (part.iov_len == 0) ++next;
        }
    }
}

void write_all(int fd, const void* input, std::size_t length) {
    iovec part{const_cast<void*>(input), length};
    write_all(fd, std::span(&part, 1));
}

// Hand a record's header, key and value to the file in one writev() call.
void write_record(int fd, const detail::EncodedRecordHeader& header, std::string_view key, std::string_view value) {
    std::array<iovec, 3> parts{{
        {const_cast<std::uint8_t*>(header.bytes.data()), header.bytes.size()},
        {const_cast<char*>(key.data()), key.size()},
        {const_cast<char*>(value.data()), value.size()},
    }};
    write_all(fd, parts);
}

// Lock a database that this opener did not create: an existing file, or one a
// competing creator published first.
void adopt_database(FileDescriptor& fd, const std::filesystem::path& path) {
    require_own_description(fd.get(), path);
    require_trusted_owner(fd.get(), path);
    lock_file(fd);
    // A creator publishes a header-only file and only then syncs the directory.
    // If it failed or died in between, the name may still not be durable, so
    // sync the directory before this Store can acknowledge anything in it.
    if (static_cast<std::uint64_t>(inspect_file(fd.get()).st_size) == detail::file_header_size) {
        sync_parent(path);
    }
}

FileDescriptor open_database(const std::filesystem::path& path, bool create_if_missing) {
    constexpr int flags = O_RDWR | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    FileDescriptor existing(open_file(path, flags));
    if (existing.get() >= 0) {
        adopt_database(existing, path);
        return existing;
    }
    if (errno != ENOENT || !create_if_missing) io_error("cannot open database");

    TemporaryFile temporary(parent_directory(path));
    (void)control_file(temporary.get(), F_SETFL,
                       control_file(temporary.get(), F_GETFL) | O_APPEND | O_NONBLOCK);
    lock_file(temporary.descriptor());
    const auto header = detail::make_file_header();
    write_all(temporary.get(), header.data(), header.size());
    sync_file(temporary.get());

    // Publish only a complete, synchronized, already-locked inode. link() is
    // atomic and refuses to replace a file created by a competing opener.
    int status;
    do { status = ::link(temporary.name(), path.c_str()); } while (status < 0 && errno == EINTR);
    if (status < 0) {
        const int failure = errno;
        temporary.remove_name();
        if (failure != EEXIST) io_error("cannot publish database", failure);
        FileDescriptor winner(open_file(path, flags));
        if (winner.get() < 0) io_error("cannot open concurrently created database");
        adopt_database(winner, path);
        return winner;
    }
    temporary.remove_name();
    sync_parent(path);
    // The same descriptor retains its lock until this Store is destroyed.
    return temporary.release_descriptor();
}

void read_exact(int fd, void* output, std::size_t length, std::uint64_t offset) {
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    if (offset > maximum || length > maximum - offset) {
        throw Error(ErrorCode::corruption, "database record exceeds supported file offsets");
    }
    auto* bytes = static_cast<char*>(output);
    std::size_t done = 0;
    while (done < length) {
        const auto result = ::pread(fd, bytes + done, length - done,
                                    static_cast<off_t>(offset + done));
        if (result < 0) {
            if (errno == EINTR) continue;
            io_error("cannot read database record");
        }
        if (result == 0) {
            throw Error(ErrorCode::corruption, "database was truncated while being read");
        }
        done += static_cast<std::size_t>(result);
    }
}

void validate_key(std::string_view key) {
    if (key.empty() || key.size() > Store::max_key_bytes) {
        throw Error(ErrorCode::invalid_argument, "key must contain 1 to 4096 bytes");
    }
}

// Run a record-format check and report a failure with the record's byte offset,
// which is where the valid records end.
template <class Check>
decltype(auto) at_record(std::uint64_t offset, Check&& check) {
    try {
        return check();
    } catch (const Error& error) {
        if (error.code() != ErrorCode::corruption) throw;
        throw Error(ErrorCode::corruption, std::string(error.what()) + " at byte " + std::to_string(offset));
    }
}

} // namespace

struct Store::Impl {
    struct Entry {
        std::uint64_t offset;
        std::uint32_t key_size;
        std::uint32_t value_size;
        std::uint32_t payload_crc;
    };
    using Index = std::map<std::string, Entry, std::less<>>;

    explicit Impl(const std::filesystem::path& path, Options options) {
        if (path.empty() || path.native().find('\0') != std::string::npos) {
            throw Error(ErrorCode::invalid_argument, "database path must be nonempty and contain no NUL bytes");
        }
        fd = open_database(path, options.create_if_missing);
        file_bytes = static_cast<std::uint64_t>(inspect_file(fd.get()).st_size);
        scan(options.repair_incomplete_tail);
    }

    void healthy() const {
        if (::getpid() != owner_pid) {
            throw Error(ErrorCode::invalid_argument,
                        "Store cannot be used after fork; close the inherited instance and open a new Store");
        }
        if (poisoned) {
            throw Error(ErrorCode::io, "database state is uncertain after a failed write; close and reopen it");
        }
        if (static_cast<std::uint64_t>(inspect_file(fd.get()).st_size) != file_bytes) {
            throw Error(ErrorCode::corruption, "database size changed outside this Store");
        }
    }

    void incomplete(std::uint64_t start, bool repair, bool zero_filled = false) {
        if (!repair) {
            throw Error(ErrorCode::incomplete_tail,
                        "incomplete final record at byte " + std::to_string(start) +
                        (zero_filled ? " (only zero bytes)" : "") +
                        "; reopen with explicit tail repair to discard " +
                        std::to_string(file_bytes - start) + " trailing bytes");
        }
        int status;
        do {
            status = ::ftruncate(fd.get(), static_cast<off_t>(start));
        } while (status < 0 && errno == EINTR);
        if (status < 0) io_error("cannot truncate incomplete final record");
        sync_file(fd.get());
        repaired = file_bytes - start;
        file_bytes = start;
    }

    // Whether every byte from start to the end of the file is zero.
    bool zero_filled_from(std::uint64_t start) const {
        std::array<std::uint8_t, 4096> block {};
        for (auto offset = start; offset < file_bytes;) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(file_bytes - offset, block.size()));
            read_exact(fd.get(), block.data(), count, offset);
            const std::span<const std::uint8_t> chunk(block.data(), count);
            if (std::any_of(chunk.begin(), chunk.end(), [](std::uint8_t byte) { return byte != 0; })) return false;
            offset += count;
        }
        return true;
    }

    // Read a record's key and value into payload, reusing its capacity, and
    // check their checksum.
    void read_payload(const detail::RecordHeader& header, std::uint64_t offset, std::string& payload) const {
        // Header decoding has already bounded both lengths before allocation.
        payload.resize(static_cast<std::size_t>(header.key_size) + header.value_size);
        read_exact(fd.get(), payload.data(), payload.size(), offset + detail::record_header_size);
        const std::string_view key(payload.data(), header.key_size);
        const std::string_view value(payload.data() + header.key_size, header.value_size);
        if (detail::payload_crc(key, value) != header.payload_crc) {
            throw Error(ErrorCode::corruption, "payload checksum mismatch at byte " + std::to_string(offset));
        }
    }

    void scan(bool repair) {
        if (file_bytes < detail::file_header_size) {
            throw Error(ErrorCode::corruption, "database header is missing or incomplete");
        }
        std::array<std::uint8_t, detail::file_header_size> file_header {};
        read_exact(fd.get(), file_header.data(), file_header.size(), 0);
        detail::validate_file_header(file_header);

        std::uint64_t position = detail::file_header_size;
        // One buffer serves every record, and a key is copied only when it
        // enters the index, so replay allocates per live key, not per record.
        std::string payload;
        while (position < file_bytes) {
            const auto remaining = file_bytes - position;
            std::array<std::uint8_t, detail::record_header_size> bytes {};
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, bytes.size()));
            read_exact(fd.get(), bytes.data(), count, position);
            // A crash can leave a file whose size covers an append that never
            // reached the disk; those blocks read back as zeros. Every record
            // starts with nonzero magic, so zeros from a record boundary to the
            // end of the file hold no record. Like an incomplete record, they
            // are an error by default and discarded only by explicit repair.
            if (bytes[0] == 0 && zero_filled_from(position)) {
                incomplete(position, repair, true);
                break;
            }
            if (count < bytes.size()) {
                at_record(position, [&] { detail::validate_partial_record_header(std::span(bytes.data(), count)); });
                incomplete(position, repair);
                break;
            }

            const auto header = at_record(position, [&] { return detail::parse_record_header(bytes); });
            const std::uint64_t record_size = detail::record_header_size +
                static_cast<std::uint64_t>(header.key_size) + header.value_size;
            if (remaining < record_size) {
                incomplete(position, repair);
                break;
            }
            read_payload(header, position, payload);
            const std::string_view key(payload.data(), header.key_size);
            const auto slot = index.lower_bound(key);
            const bool present = slot != index.end() && slot->first == key;
            if (present) live_value_bytes -= slot->second.value_size;
            if (header.kind == detail::Kind::put) {
                ++put_records;
                const Entry entry{position, header.key_size, header.value_size, header.payload_crc};
                if (present) slot->second = entry;
                else index.emplace_hint(slot, std::string(key), entry);
                live_value_bytes += header.value_size;
            } else {
                ++erase_records;
                if (present) index.erase(slot);
            }
            ++record_count;
            position += record_size;
        }
    }

    Entry append(detail::Kind kind, std::string_view key, std::string_view value) {
        const auto header = detail::make_record_header(kind, key, value);
        const auto size = static_cast<std::uint64_t>(header.bytes.size()) + key.size() + value.size();
        const auto limit = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        if (size > limit - file_bytes) {
            throw Error(ErrorCode::io, "database would exceed supported file size");
        }
        const Entry entry{file_bytes, header.fields.key_size, header.fields.value_size,
                          header.fields.payload_crc};
        try {
            write_record(fd.get(), header, key, value);
            sync_file(fd.get());
        } catch (...) {
            poisoned = true;
            discard_unacknowledged();
            throw;
        }
        // Callers commit index/statistics changes only after synchronization.
        return entry;
    }

    // After a failed append, cut the file back to the last acknowledged record.
    // Otherwise the next open would adopt the failed record from the page cache
    // (Linux keeps pages it failed to write back, readable but never retried)
    // and acknowledge later records after it. The Store stays poisoned and the
    // original error is reported; if this cleanup fails too, the outcome of the
    // failed operation remains uncertain.
    void discard_unacknowledged() noexcept {
        int status;
        do {
            status = ::ftruncate(fd.get(), static_cast<off_t>(file_bytes));
        } while (status < 0 && errno == EINTR);
        if (status < 0) return;
        try {
            sync_file(fd.get());
        } catch (...) {
            // Later opens still see the truncated size; only its durability is unknown.
        }
    }

    // A map node holding key, allocated before an append so that linking it into
    // the index afterwards cannot fail.
    static Index::node_type detached_node(std::string_view key) {
        Index staging;
        staging.emplace(std::string(key), Entry{});
        return staging.extract(staging.begin());
    }

    // flock is shared across fork, while each process has its own index copy.
    const pid_t owner_pid = ::getpid();
    FileDescriptor fd;
    Index index;
    std::uint64_t record_count = 0;
    std::uint64_t put_records = 0;
    std::uint64_t erase_records = 0;
    std::uint64_t file_bytes = 0;
    std::uint64_t live_value_bytes = 0;
    std::uint64_t repaired = 0;
    bool poisoned = false;
};

Store::Store(const std::filesystem::path& path, Options options)
    : impl_(std::make_unique<Impl>(path, options)) {}

Store::~Store() = default;

void Store::put(std::string_view key, std::string_view value) {
    validate_key(key);
    if (value.size() > max_value_bytes) {
        throw Error(ErrorCode::invalid_argument, "value exceeds the 1048576-byte limit");
    }
    impl_->healthy();
    // Allocate everything the index needs before appending: once the record is
    // synced, the put must return normally rather than report a committed write
    // as failed. A new key's map node is built now and linked in afterwards.
    auto& index = impl_->index;
    const auto found = index.find(key);
    Impl::Index::node_type node;
    if (found == index.end()) node = Impl::detached_node(key);
    const std::uint64_t previous_size = found == index.end() ? 0 : found->second.value_size;
    const auto entry = impl_->append(detail::Kind::put, key, value);
    if (node) {
        node.mapped() = entry;
        index.insert(std::move(node));
    } else {
        found->second = entry;
    }
    impl_->file_bytes += detail::record_header_size + key.size() + value.size();
    ++impl_->record_count;
    ++impl_->put_records;
    impl_->live_value_bytes = impl_->live_value_bytes - previous_size + value.size();
}

std::optional<std::string> Store::get(std::string_view key) const {
    validate_key(key);
    impl_->healthy();
    const auto found = impl_->index.find(key);
    if (found == impl_->index.end()) return std::nullopt;
    const auto& entry = found->second;
    std::array<std::uint8_t, detail::record_header_size> bytes {};
    read_exact(impl_->fd.get(), bytes.data(), bytes.size(), entry.offset);
    const auto header = detail::parse_record_header(bytes);
    if (header.kind != detail::Kind::put || header.key_size != entry.key_size ||
        header.value_size != entry.value_size || header.payload_crc != entry.payload_crc) {
        throw Error(ErrorCode::corruption, "indexed record changed outside this Store");
    }
    std::string payload;
    impl_->read_payload(header, entry.offset, payload);
    if (std::string_view(payload.data(), header.key_size) != key) {
        throw Error(ErrorCode::corruption, "indexed key changed outside this Store");
    }
    payload.erase(0, header.key_size); // Keep the value without copying it to a second string.
    return payload;
}

bool Store::erase(std::string_view key) {
    validate_key(key);
    impl_->healthy();
    const auto found = impl_->index.find(key);
    if (found == impl_->index.end()) return false;
    const auto previous_size = found->second.value_size;
    (void)impl_->append(detail::Kind::erase, key, {});
    impl_->index.erase(found);
    impl_->file_bytes += detail::record_header_size + key.size();
    ++impl_->record_count;
    ++impl_->erase_records;
    impl_->live_value_bytes -= previous_size;
    return true;
}

std::vector<std::string> Store::keys() const {
    impl_->healthy();
    std::vector<std::string> result;
    result.reserve(impl_->index.size());
    for (const auto& [key, entry] : impl_->index) {
        (void)entry;
        result.push_back(key);
    }
    return result;
}

std::vector<std::string> Store::keys_page(std::string_view prefix, std::size_t limit,
                                        std::optional<std::string_view> after) const {
    if (prefix.size() > max_key_bytes || limit == 0 || limit > max_page_keys) {
        throw Error(ErrorCode::invalid_argument, "prefix must be at most 4096 bytes and page limit must be 1 to 1000");
    }
    if (after) validate_key(*after);
    impl_->healthy();
    auto position = after && *after >= prefix
        ? impl_->index.upper_bound(*after) : impl_->index.lower_bound(prefix);
    std::vector<std::string> result;
    result.reserve(limit);
    for (; position != impl_->index.end() && result.size() < limit; ++position) {
        if (!position->first.starts_with(prefix)) break;
        result.push_back(position->first);
    }
    return result;
}

Stats Store::stats() const {
    impl_->healthy();
    return {static_cast<std::uint64_t>(impl_->index.size()), impl_->record_count,
            impl_->file_bytes, impl_->live_value_bytes};
}

InspectionReport Store::inspect() const {
    impl_->healthy();
    InspectionReport report{};
    report.file_bytes = impl_->file_bytes;
    report.header_bytes = detail::file_header_size;
    report.record_count = impl_->record_count;
    report.put_records = impl_->put_records;
    report.erase_records = impl_->erase_records;
    report.live_keys = static_cast<std::uint64_t>(impl_->index.size());
    report.superseded_records =
        impl_->put_records >= report.live_keys ? (impl_->put_records - report.live_keys) : 0;
    report.live_value_bytes = impl_->live_value_bytes;

    std::uint64_t live_k_bytes = 0;
    std::uint64_t live_rec_bytes = detail::file_header_size;
    bool first = true;
    for (const auto& [key, entry] : impl_->index) {
        live_k_bytes += entry.key_size;
        live_rec_bytes += detail::record_header_size + entry.key_size + entry.value_size;
        if (first || entry.key_size < report.min_key_bytes) report.min_key_bytes = entry.key_size;
        if (first || entry.key_size > report.max_key_bytes) report.max_key_bytes = entry.key_size;
        if (first || entry.value_size < report.min_value_bytes) report.min_value_bytes = entry.value_size;
        if (first || entry.value_size > report.max_value_bytes) {
            report.max_value_bytes = entry.value_size;
            report.largest_key = key;
            report.largest_value_bytes = entry.value_size;
        }
        if (entry.value_size <= 64U) ++report.values_le_64;
        else if (entry.value_size <= 1024U) ++report.values_le_1024;
        else ++report.values_gt_1024;
        first = false;
    }
    report.live_key_bytes = live_k_bytes;
    report.live_record_bytes = live_rec_bytes;
    report.reclaimable_bytes =
        report.file_bytes >= live_rec_bytes ? (report.file_bytes - live_rec_bytes) : 0;
    const auto denom = std::max<std::uint64_t>(1, live_rec_bytes);
    report.space_amplification = static_cast<double>(report.file_bytes) / static_cast<double>(denom);
    return report;
}

void Store::snapshot(const std::filesystem::path& destination) const {
    if (destination.empty() || destination.native().find('\0') != std::string::npos) {
        throw Error(ErrorCode::invalid_argument, "snapshot path must be nonempty and contain no NUL bytes");
    }
    impl_->healthy();
    // The private file is close-on-exec from creation, like a new database's.
    TemporaryFile temporary(parent_directory(destination));
    lock_file(temporary.descriptor());
    const auto file_header = detail::make_file_header();
    write_all(temporary.get(), file_header.data(), file_header.size());
    // The source lock stays held. One bounded value is materialized at a time,
    // and get() rechecks its header, key and payload checksum before copying.
    for (const auto& [key, entry] : impl_->index) {
        (void)entry;
        const auto value = get(key).value();
        write_record(temporary.get(), detail::make_record_header(detail::Kind::put, key, value), key, value);
    }
    sync_file(temporary.get());
    int status;
    do { status = ::link(temporary.name(), destination.c_str()); } while (status < 0 && errno == EINTR);
    if (status < 0) io_error("cannot publish snapshot (destination must not exist)");
    temporary.remove_name();
    sync_parent(destination);
}

std::uint64_t Store::repaired_bytes() const noexcept { return impl_->repaired; }

} // namespace pocketkv
