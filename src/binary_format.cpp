#include "binary_format.hpp"

namespace minidb::detail {
namespace {

/// Appends `byte_count` bytes of `value`, least significant byte first.
template<typename Unsigned>
void append_little_endian(std::string& out, Unsigned value, int byte_count) {
    for (int index = 0; index < byte_count; ++index) {
        out.push_back(static_cast<char>((value >> (index * 8)) & 0xFFU));
    }
}

/// Reads `byte_count` bytes as a little-endian integer.
template<typename Unsigned>
[[nodiscard]] Unsigned read_little_endian(const char* bytes, int byte_count) noexcept {
    Unsigned value = 0;
    for (int index = byte_count - 1; index >= 0; --index) {
        value = static_cast<Unsigned>(value << 8);
        value = static_cast<Unsigned>(
            value | static_cast<Unsigned>(static_cast<unsigned char>(bytes[index])));
    }
    return value;
}

}  // namespace

void append_u16(std::string& out, std::uint16_t value) {
    append_little_endian(out, value, 2);
}

void append_u32(std::string& out, std::uint32_t value) {
    append_little_endian(out, value, 4);
}

void append_u64(std::string& out, std::uint64_t value) {
    append_little_endian(out, value, 8);
}

std::uint16_t read_u16(const char* bytes) noexcept {
    return read_little_endian<std::uint16_t>(bytes, 2);
}

std::uint32_t read_u32(const char* bytes) noexcept {
    return read_little_endian<std::uint32_t>(bytes, 4);
}

std::uint64_t read_u64(const char* bytes) noexcept {
    return read_little_endian<std::uint64_t>(bytes, 8);
}

std::uint32_t crc32_update(std::uint32_t crc, const char* data, std::size_t length) noexcept {
    for (std::size_t index = 0; index < length; ++index) {
        crc ^= static_cast<std::uint32_t>(static_cast<unsigned char>(data[index]));
        for (int bit = 0; bit < 8; ++bit) {
            crc = ((crc & 1U) != 0U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
        }
    }
    return crc;
}

std::uint32_t crc32_update(std::uint32_t crc, std::string_view data) noexcept {
    return crc32_update(crc, data.data(), data.size());
}

std::uint32_t crc32_finish(std::uint32_t crc) noexcept {
    return crc ^ 0xFFFFFFFFU;
}

}  // namespace minidb::detail
