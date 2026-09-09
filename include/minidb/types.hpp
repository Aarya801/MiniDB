#ifndef MINIDB_TYPES_HPP
#define MINIDB_TYPES_HPP

#include <string>

namespace minidb {

/// Keys and values are opaque byte strings. std::string is used rather than a
/// dedicated byte-buffer type because it already owns its storage, is
/// null-safe for binary data (it stores a length, not a terminator), and
/// interoperates with the standard library everywhere we need it.
using Key = std::string;
using Value = std::string;

}  // namespace minidb

#endif  // MINIDB_TYPES_HPP
