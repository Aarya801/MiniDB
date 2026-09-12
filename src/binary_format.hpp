#ifndef MINIDB_SRC_BINARY_FORMAT_HPP
#define MINIDB_SRC_BINARY_FORMAT_HPP

// Byte-level helpers shared by the snapshot and the write-ahead log.
//
// This header lives in src/ rather than include/ because it is not part of
// MiniDB's public interface: nothing outside the engine needs to encode a
// little-endian integer or checksum a buffer. The snapshot format document
// notes that Milestone 3 kept these private to storage.cpp and that the log
// would want them too; this is that promotion, so the two formats cannot
// drift apart in how they encode a length or compute a CRC.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace minidb::detail {

// -------------------------------------------------------------------------
// Little-endian integers
//
// Encoded one byte at a time, least significant first, rather than by copying
// the bytes of the in-memory value. That keeps every MiniDB file readable
// regardless of the CPU's byte order, and is why no format code needs a
// reinterpret_cast.
// -------------------------------------------------------------------------

void append_u16(std::string& out, std::uint16_t value);
void append_u32(std::string& out, std::uint32_t value);
void append_u64(std::string& out, std::uint64_t value);

[[nodiscard]] std::uint16_t read_u16(const char* bytes) noexcept;
[[nodiscard]] std::uint32_t read_u32(const char* bytes) noexcept;
[[nodiscard]] std::uint64_t read_u64(const char* bytes) noexcept;

// -------------------------------------------------------------------------
// CRC-32
//
// The standard CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320) used by
// both file formats. Computed without a lookup table: eight shifts per byte
// is nothing beside the cost of the I/O, and it keeps the implementation
// short enough to read.
//
// Start from kCrcInitial, feed the bytes through crc32_update in order, and
// finish with crc32_finish. It detects accidental damage -- notably a flipped
// bit inside a key or value, which no structural check can see. It is not a
// security measure: anyone editing a file can recompute it.
// -------------------------------------------------------------------------

inline constexpr std::uint32_t kCrcInitial = 0xFFFFFFFFU;

[[nodiscard]] std::uint32_t crc32_update(std::uint32_t crc, const char* data,
                                         std::size_t length) noexcept;
[[nodiscard]] std::uint32_t crc32_update(std::uint32_t crc, std::string_view data) noexcept;
[[nodiscard]] std::uint32_t crc32_finish(std::uint32_t crc) noexcept;

}  // namespace minidb::detail

#endif  // MINIDB_SRC_BINARY_FORMAT_HPP
