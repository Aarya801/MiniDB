#ifndef MINIDB_TYPES_HPP
#define MINIDB_TYPES_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace minidb {

/// Keys and values are opaque byte strings. std::string is used rather than a
/// dedicated byte-buffer type because it already owns its storage, is
/// null-safe for binary data (it stores a length, not a terminator), and
/// interoperates with the standard library everywhere we need it.
using Key = std::string;
using Value = std::string;

/// Size limits on stored data.
///
/// A database must bound what it accepts, and must bound what it is willing
/// to believe from a file it did not write. The key and value limits are
/// enforced by Database::set and again when a snapshot is read; the snapshot
/// limits are enforced by StorageManager. The write-ahead log adds its own
/// record bound in Milestone 4.
namespace limits {

/// Maximum length of a key, in bytes.
inline constexpr std::size_t kMaxKeySize = 1024;

/// Maximum length of a value, in bytes.
inline constexpr std::size_t kMaxValueSize = 1024 * 1024;  // 1 MiB

/// Maximum number of records a snapshot may claim to hold.
///
/// Enforced when reading, where the count comes from a file that may be
/// damaged. A corrupt count of 2^64-1 must be refused before it reaches a
/// reserve() call or a loop bound.
inline constexpr std::uint64_t kMaxRecordCount = 10'000'000;

/// Maximum size of a snapshot file, in bytes.
///
/// Checked before a single byte is parsed, so an absurdly large or truncated
/// file is rejected up front rather than during a long read.
inline constexpr std::uintmax_t kMaxSnapshotSize = 256ULL * 1024 * 1024;  // 256 MiB

}  // namespace limits

}  // namespace minidb

#endif  // MINIDB_TYPES_HPP
