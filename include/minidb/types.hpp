#ifndef MINIDB_TYPES_HPP
#define MINIDB_TYPES_HPP

#include <cstddef>
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
/// A database must bound what it accepts. These two are enforced by
/// Database::set; the record and log limits that guard data read back from
/// disk arrive with the storage format in Milestone 3.
namespace limits {

/// Maximum length of a key, in bytes.
inline constexpr std::size_t kMaxKeySize = 1024;

/// Maximum length of a value, in bytes.
inline constexpr std::size_t kMaxValueSize = 1024 * 1024;  // 1 MiB

}  // namespace limits

}  // namespace minidb

#endif  // MINIDB_TYPES_HPP
