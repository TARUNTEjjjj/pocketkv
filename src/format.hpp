#pragma once

#include "pocketkv/store.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace pocketkv::detail {

inline constexpr std::size_t file_header_size = 16;
inline constexpr std::size_t record_header_size = 24;
enum class Kind : std::uint8_t { put = 1, erase = 2 };
struct RecordHeader {
    Kind kind;
    std::uint32_t key_size;
    std::uint32_t value_size;
    std::uint32_t payload_crc;
};

[[nodiscard]] std::array<std::uint8_t, file_header_size> make_file_header();
void validate_file_header(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::array<std::uint8_t, record_header_size> make_record_header(Kind kind, std::string_view key, std::string_view value);
[[nodiscard]] RecordHeader parse_record_header(std::span<const std::uint8_t> bytes);
// Validate all available fixed fields and completed lengths of a short header.
// Used only to classify a final short suffix; it does not certify missing bytes.
void validate_partial_record_header(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] std::uint32_t payload_crc(std::string_view key, std::string_view value) noexcept;

} // namespace pocketkv::detail
