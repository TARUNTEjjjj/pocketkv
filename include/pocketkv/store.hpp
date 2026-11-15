#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pocketkv {

enum class ErrorCode { invalid_argument, io, locked, corruption, incomplete_tail };

class Error : public std::runtime_error {
public:
    Error(ErrorCode code, std::string message) : std::runtime_error(std::move(message)), code_(code) {}
    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
private:
    ErrorCode code_;
};

struct Options {
    bool create_if_missing = true;
    bool repair_incomplete_tail = false;
};

struct Stats {
    std::uint64_t key_count;
    std::uint64_t record_count;
    std::uint64_t file_bytes;
    std::uint64_t live_value_bytes;
};

// Exclusive advisory lock for this object's lifetime. The object is not
// thread-safe: callers must serialize operations. Each mutation fsyncs its
// append before updating the in-memory index or returning success.
// Data operations reject inherited instances in forked child processes.
class Store {
public:
    static constexpr std::size_t max_key_bytes = 4096;
    static constexpr std::size_t max_value_bytes = 1024 * 1024;
    static constexpr std::size_t max_page_keys = 1000;

    explicit Store(const std::filesystem::path& path, Options options = {});
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store&&) = delete;

    void put(std::string_view key, std::string_view value);
    [[nodiscard]] std::optional<std::string> get(std::string_view key) const;
    // No record is written if the key is absent.
    bool erase(std::string_view key);
    [[nodiscard]] std::vector<std::string> keys() const;
    // Bytewise ordered prefix matches, strictly after the optional raw-key
    // cursor. Empty prefix matches all keys; limit must be 1..max_page_keys.
    // Pages are not snapshots: mutations between calls affect later results.
    [[nodiscard]] std::vector<std::string> keys_page(
        std::string_view prefix, std::size_t limit = 100,
        std::optional<std::string_view> after = std::nullopt) const;
    [[nodiscard]] Stats stats() const;
    // Write a compact, independently usable snapshot to a new path. Includes
    // only live keys, validates each value, and never replaces a destination.
    // Publication is atomic; success includes file and directory fsync.
    void snapshot(const std::filesystem::path& destination) const;
    [[nodiscard]] std::uint64_t repaired_bytes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pocketkv
