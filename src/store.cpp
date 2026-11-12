#include "pocketkv/store.hpp"
#include "format.hpp"

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
#include <unistd.h>
#include <utility>

namespace pocketkv {
namespace {

class FileDescriptor {
public:
    explicit FileDescriptor(int value = -1) noexcept : value_(value) {}
    ~FileDescriptor() { if (value_ >= 0) ::close(value_); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) ::close(value_);
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    [[nodiscard]] int get() const noexcept { return value_; }
private:
    int value_;
};

[[noreturn]] void io_error(std::string_view operation, int code = errno) {
    throw Error(ErrorCode::io, std::string(operation) + ": " + std::strerror(code));
}

// A process may start with stdin, stdout or stderr closed (a shell's `>&-`).
// open() and mkstemp() return the lowest free descriptor, so a database could
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

class TemporaryFile {
public:
    explicit TemporaryFile(const std::filesystem::path& directory)
        : name_((directory / ".pocketkv-XXXXXX").string()) {
        // mkstemp creates a private 0600 file with O_EXCL. All potentially
        // allocating work precedes creation so no exception can lose its name.
        const int created = ::mkstemp(name_.data());
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
        if (owns_name_) {
            int status;
            do { status = ::unlink(name_.c_str()); } while (status < 0 && errno == EINTR);
        }
    }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    [[nodiscard]] int get() const noexcept { return fd_.get(); }
    [[nodiscard]] const char* name() const noexcept { return name_.c_str(); }
    [[nodiscard]] FileDescriptor release_descriptor() noexcept { return std::move(fd_); }
    void remove_name() {
        int status;
        do { status = ::unlink(name_.c_str()); } while (status < 0 && errno == EINTR);
        if (status < 0) io_error("cannot remove private database file");
        owns_name_ = false;
    }
private:
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

void lock_file(int fd) {
    (void)inspect_file(fd); // Reject FIFOs/devices before attempting a lock.
    int status;
    do { status = ::flock(fd, LOCK_EX | LOCK_NB); } while (status < 0 && errno == EINTR);
    if (status < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            throw Error(ErrorCode::locked, "database is already open; PocketKV requires an exclusive lock");
        }
        io_error("cannot lock database");
    }
}

void sync_file(int fd) {
    int status;
    do {
        status = ::fsync(fd);
    } while (status < 0 && errno == EINTR);
    if (status < 0) io_error("cannot synchronize database");
}

void sync_parent(const std::filesystem::path& path) {
    auto parent = path.parent_path();
    if (parent.empty()) parent = ".";
    FileDescriptor directory(open_file(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.get() < 0) io_error("cannot open database directory");
    int status;
    do {
        status = ::fsync(directory.get());
    } while (status < 0 && errno == EINTR);
    if (status < 0) io_error("cannot synchronize database directory");
}

void write_all(int fd, const void* input, std::size_t length) {
    const auto* bytes = static_cast<const char*>(input);
    std::size_t done = 0;
    while (done < length) {
        const auto result = ::write(fd, bytes + done, length - done);
        if (result < 0) {
            if (errno == EINTR) continue;
            io_error("cannot append database record");
        }
        if (result == 0) io_error("database append made no progress", EIO);
        done += static_cast<std::size_t>(result);
    }
}

FileDescriptor open_database(const std::filesystem::path& path, bool create_if_missing) {
    constexpr int flags = O_RDWR | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    FileDescriptor existing(open_file(path, flags));
    if (existing.get() >= 0) {
        lock_file(existing.get());
        return existing;
    }
    if (errno != ENOENT || !create_if_missing) io_error("cannot open database");

    auto parent = path.parent_path();
    if (parent.empty()) parent = ".";
    TemporaryFile temporary(parent);
    (void)control_file(temporary.get(), F_SETFD,
                       control_file(temporary.get(), F_GETFD) | FD_CLOEXEC);
    (void)control_file(temporary.get(), F_SETFL,
                       control_file(temporary.get(), F_GETFL) | O_APPEND | O_NONBLOCK);
    lock_file(temporary.get());
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
        lock_file(winner.get());
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

} // namespace

struct Store::Impl {
    struct Entry {
        std::uint64_t offset;
        std::uint32_t key_size;
        std::uint32_t value_size;
        std::uint32_t payload_crc;
    };

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

    void incomplete(std::uint64_t start, bool repair) {
        if (!repair) {
            throw Error(ErrorCode::incomplete_tail,
                        "incomplete final record at byte " + std::to_string(start) +
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

    std::string read_payload(const detail::RecordHeader& header, std::uint64_t offset) const {
        // Header decoding has already bounded both lengths before allocation.
        const auto payload_size = static_cast<std::size_t>(header.key_size) + header.value_size;
        std::string payload(payload_size, '\0');
        read_exact(fd.get(), payload.data(), payload.size(), offset + detail::record_header_size);
        const std::string_view key(payload.data(), header.key_size);
        const std::string_view value(payload.data() + header.key_size, header.value_size);
        if (detail::payload_crc(key, value) != header.payload_crc) {
            throw Error(ErrorCode::corruption, "payload checksum mismatch at byte " + std::to_string(offset));
        }
        return payload;
    }

    void scan(bool repair) {
        if (file_bytes < detail::file_header_size) {
            throw Error(ErrorCode::corruption, "database header is missing or incomplete");
        }
        std::array<std::uint8_t, detail::file_header_size> file_header {};
        read_exact(fd.get(), file_header.data(), file_header.size(), 0);
        detail::validate_file_header(file_header);

        std::uint64_t position = detail::file_header_size;
        while (position < file_bytes) {
            const auto remaining = file_bytes - position;
            std::array<std::uint8_t, detail::record_header_size> bytes {};
            if (remaining < bytes.size()) {
                const auto count = static_cast<std::size_t>(remaining);
                read_exact(fd.get(), bytes.data(), count, position);
                detail::validate_partial_record_header(std::span(bytes.data(), count));
                incomplete(position, repair);
                break;
            }

            read_exact(fd.get(), bytes.data(), bytes.size(), position);
            const auto header = detail::parse_record_header(bytes);
            const std::uint64_t record_size = detail::record_header_size +
                static_cast<std::uint64_t>(header.key_size) + header.value_size;
            if (remaining < record_size) {
                incomplete(position, repair);
                break;
            }
            const auto payload = read_payload(header, position);
            std::string key(payload.data(), header.key_size);
            const auto found = index.find(key);
            if (found != index.end()) live_value_bytes -= found->second.value_size;
            if (header.kind == detail::Kind::put) {
                index.insert_or_assign(std::move(key), Entry{position, header.key_size,
                                                           header.value_size, header.payload_crc});
                live_value_bytes += header.value_size;
            } else if (found != index.end()) {
                index.erase(found);
            }
            ++record_count;
            position += record_size;
        }
    }

    Entry append(detail::Kind kind, std::string_view key, std::string_view value) {
        const auto header = detail::make_record_header(kind, key, value);
        const auto metadata = detail::parse_record_header(header);
        const auto size = static_cast<std::uint64_t>(header.size()) + key.size() + value.size();
        const auto limit = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        if (size > limit - file_bytes) {
            throw Error(ErrorCode::io, "database would exceed supported file size");
        }
        const Entry entry{file_bytes, metadata.key_size, metadata.value_size, metadata.payload_crc};
        try {
            write_all(fd.get(), header.data(), header.size());
            write_all(fd.get(), key.data(), key.size());
            write_all(fd.get(), value.data(), value.size());
            sync_file(fd.get());
        } catch (...) {
            poisoned = true;
            throw;
        }
        // Callers commit index/statistics changes only after synchronization.
        return entry;
    }

    // flock is shared across fork, while each process has its own index copy.
    const pid_t owner_pid = ::getpid();
    FileDescriptor fd;
    std::map<std::string, Entry, std::less<>> index;
    std::uint64_t record_count = 0;
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
    // Allocate the owned key before starting an append. If a subsequent map
    // node allocation fails after fsync, poison the instance rather than expose
    // an index that silently disagrees with the successfully written file.
    std::string owned_key(key);
    const auto found = impl_->index.find(key);
    const std::uint64_t previous_size = found == impl_->index.end() ? 0 : found->second.value_size;
    const auto entry = impl_->append(detail::Kind::put, key, value);
    try {
        impl_->index.insert_or_assign(std::move(owned_key), entry);
    } catch (...) {
        impl_->poisoned = true;
        throw;
    }
    impl_->file_bytes += detail::record_header_size + key.size() + value.size();
    ++impl_->record_count;
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
    const auto payload = impl_->read_payload(header, entry.offset);
    if (std::string_view(payload.data(), header.key_size) != key) {
        throw Error(ErrorCode::corruption, "indexed key changed outside this Store");
    }
    return payload.substr(header.key_size, header.value_size);
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

void Store::snapshot(const std::filesystem::path& destination) const {
    if (destination.empty() || destination.native().find('\0') != std::string::npos) {
        throw Error(ErrorCode::invalid_argument, "snapshot path must be nonempty and contain no NUL bytes");
    }
    impl_->healthy();
    auto parent = destination.parent_path();
    if (parent.empty()) parent = ".";
    TemporaryFile temporary(parent);
    (void)control_file(temporary.get(), F_SETFD,
                       control_file(temporary.get(), F_GETFD) | FD_CLOEXEC);
    lock_file(temporary.get());
    const auto file_header = detail::make_file_header();
    write_all(temporary.get(), file_header.data(), file_header.size());
    // The source lock stays held. One bounded value is materialized at a time,
    // and get() rechecks its header, key and payload checksum before copying.
    for (const auto& [key, entry] : impl_->index) {
        (void)entry;
        const auto value = get(key).value();
        const auto header = detail::make_record_header(detail::Kind::put, key, value);
        write_all(temporary.get(), header.data(), header.size());
        write_all(temporary.get(), key.data(), key.size());
        write_all(temporary.get(), value.data(), value.size());
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
