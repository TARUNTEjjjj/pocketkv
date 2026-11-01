#include "format.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace pocketkv::detail {
namespace {

constexpr std::array<std::uint8_t, 8> file_magic{'P', 'O', 'C', 'K', 'E', 'T', 'K', 'V'};
constexpr std::array<std::uint8_t, 4> record_magic{'P', 'K', 'V', 'R'};
constexpr std::uint32_t format_version = 1;

constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::size_t index = 0; index < table.size(); ++index) {
        auto value = static_cast<std::uint32_t>(index);
        for (unsigned int bit = 0; bit < 8; ++bit) {
            value = (value >> 1) ^ ((value & 1U) != 0 ? 0xedb88320U : 0U);
        }
        table[index] = value;
    }
    return table;
}

constexpr auto crc_table = make_crc_table();

std::uint32_t update_crc(const std::uint32_t state, const std::uint8_t byte) noexcept {
    return (state >> 8) ^ crc_table[(state ^ byte) & 0xffU];
}

std::uint32_t read_u32(const std::span<const std::uint8_t> bytes,
                       const std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void write_u32(const std::span<std::uint8_t> bytes, const std::size_t offset,
               const std::uint32_t value) noexcept {
    for (unsigned int index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xffU);
    }
}

[[noreturn]] void corrupt(const char* message) {
    throw Error(ErrorCode::corruption, message);
}

void validate_length_prefix(const std::span<const std::uint8_t> bytes,
                            const std::size_t offset, const std::uint32_t minimum,
                            const std::uint32_t maximum, const char* message) {
    if (bytes.size() <= offset) return;
    const auto count = std::min<std::size_t>(bytes.size() - offset, 4);
    std::uint64_t candidate = 0;
    for (std::size_t index = 0; index < count; ++index) {
        candidate |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    // Known little-endian bytes constrain every completion to this residue.
    // Find the smallest completion at or above the allowed minimum. A wider
    // type also handles a complete zero key length without shifting by 32 in
    // uint32_t. If this candidate exceeds the maximum, every completion fails.
    if (candidate < minimum) {
        const auto stride = std::uint64_t{1} << (count * 8U);
        candidate += ((minimum - candidate + stride - 1) / stride) * stride;
    }
    if (candidate > maximum) corrupt(message);
}

void validate_record_fields(const std::span<const std::uint8_t> bytes) {
    // The caller decides whether this is a full checksummed header or merely
    // an incomplete final prefix. Inspect only present bytes, and reject a
    // partial length when none of its possible completions would be valid.
    for (std::size_t index = 0; index < std::min(bytes.size(), record_magic.size()); ++index) {
        if (bytes[index] != record_magic[index]) {
            corrupt("invalid record magic");
        }
    }
    if (bytes.size() >= 5 && bytes[4] != static_cast<std::uint8_t>(Kind::put) &&
        bytes[4] != static_cast<std::uint8_t>(Kind::erase)) {
        corrupt("invalid record operation");
    }
    for (std::size_t index = 5; index < std::min<std::size_t>(bytes.size(), 8); ++index) {
        if (bytes[index] != 0) {
            corrupt("unsupported record flags or reserved fields");
        }
    }
    validate_length_prefix(bytes, 8, 1, Store::max_key_bytes, "invalid record key length");
    if (bytes.size() > 12) {
        const bool erase = bytes[4] == static_cast<std::uint8_t>(Kind::erase);
        validate_length_prefix(bytes, 12, 0, erase ? 0 : Store::max_value_bytes,
                               erase ? "delete record contains a value" : "invalid record value length");
    }
}

} // namespace

std::uint32_t crc32(const std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t state = 0xffffffffU;
    for (const auto byte : bytes) {
        state = update_crc(state, byte);
    }
    return state ^ 0xffffffffU;
}

std::uint32_t payload_crc(const std::string_view key, const std::string_view value) noexcept {
    std::uint32_t state = 0xffffffffU;
    for (const char byte : key) {
        state = update_crc(state, static_cast<std::uint8_t>(static_cast<unsigned char>(byte)));
    }
    for (const char byte : value) {
        state = update_crc(state, static_cast<std::uint8_t>(static_cast<unsigned char>(byte)));
    }
    return state ^ 0xffffffffU;
}

std::array<std::uint8_t, file_header_size> make_file_header() {
    std::array<std::uint8_t, file_header_size> bytes{};
    std::copy(file_magic.begin(), file_magic.end(), bytes.begin());
    write_u32(bytes, 8, format_version);
    write_u32(bytes, 12, crc32(std::span<const std::uint8_t>(bytes).first(12)));
    return bytes;
}

void validate_file_header(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != file_header_size) {
        corrupt("incomplete or invalid database header");
    }
    if (!std::equal(file_magic.begin(), file_magic.end(), bytes.begin())) {
        corrupt("invalid database magic");
    }
    if (read_u32(bytes, 12) != crc32(bytes.first(12))) {
        corrupt("database header checksum mismatch");
    }
    if (read_u32(bytes, 8) != format_version) {
        corrupt("unsupported database format version");
    }
}

std::array<std::uint8_t, record_header_size> make_record_header(
    const Kind kind, const std::string_view key, const std::string_view value) {
    if (kind != Kind::put && kind != Kind::erase) {
        throw Error(ErrorCode::invalid_argument, "invalid record operation");
    }
    if (key.empty() || key.size() > Store::max_key_bytes) {
        throw Error(ErrorCode::invalid_argument, "key must contain 1 to 4096 bytes");
    }
    if (value.size() > Store::max_value_bytes || (kind == Kind::erase && !value.empty())) {
        throw Error(ErrorCode::invalid_argument, "value is too large or invalid for a delete record");
    }
    std::array<std::uint8_t, record_header_size> bytes{};
    std::copy(record_magic.begin(), record_magic.end(), bytes.begin());
    bytes[4] = static_cast<std::uint8_t>(kind);
    write_u32(bytes, 8, static_cast<std::uint32_t>(key.size()));
    write_u32(bytes, 12, static_cast<std::uint32_t>(value.size()));
    write_u32(bytes, 16, payload_crc(key, value));
    write_u32(bytes, 20, crc32(std::span<const std::uint8_t>(bytes).first(20)));
    return bytes;
}

RecordHeader parse_record_header(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != record_header_size) {
        corrupt("invalid record header size");
    }
    // A length field corrupted into a plausible larger value must not turn a
    // complete damaged record into an apparently recoverable incomplete tail.
    if (read_u32(bytes, 20) != crc32(bytes.first(20))) {
        corrupt("record header checksum mismatch");
    }
    validate_record_fields(bytes);
    return {static_cast<Kind>(bytes[4]), read_u32(bytes, 8), read_u32(bytes, 12), read_u32(bytes, 16)};
}

void validate_partial_record_header(const std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() >= record_header_size) {
        corrupt("expected a nonempty incomplete record header");
    }
    validate_record_fields(bytes);
    if (bytes.size() > 20) {
        const auto expected = crc32(bytes.first(20));
        for (std::size_t index = 20; index < bytes.size(); ++index) {
            const auto shift = static_cast<unsigned int>((index - 20) * 8);
            if (bytes[index] != static_cast<std::uint8_t>((expected >> shift) & 0xffU)) {
                corrupt("partial record header checksum mismatch");
            }
        }
    }
}

} // namespace pocketkv::detail
