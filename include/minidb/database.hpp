#ifndef MINIDB_DATABASE_HPP
#define MINIDB_DATABASE_HPP

#include <cstddef>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "minidb/result.hpp"
#include "minidb/types.hpp"

namespace minidb {

/// An in-memory key-value store.
///
/// This is the whole database engine as of Milestone 1: entries live in a
/// std::unordered_map and are lost when the process exits. Persistence
/// arrives in Milestone 3, and the map itself is replaced by a hand-written
/// hash table in Milestone 2. The public interface below is meant to survive
/// both changes unchanged.
///
/// The class knows nothing about terminals or command syntax. Callers hand it
/// keys and values and receive a Result; formatting replies is the CLI's job.
///
/// Complexity, as provided by std::unordered_map:
///
///   set     average O(1), worst case O(n)
///   get     average O(1), worst case O(n)
///   remove  average O(1), worst case O(n)
///   exists  average O(1), worst case O(n)
///   keys    O(n)
///   clear   O(n)
///   size    O(1)
///
/// The averages are expected values, not guarantees. A lookup degrades toward
/// O(n) when many keys collide into one bucket; the standard library keeps
/// this rare by rehashing to bound the load factor, but adversarial or
/// unlucky key sets can still cause it.
class Database {
public:
    /// Stores `value` under `key`, replacing any existing entry.
    /// Fails with InvalidArgument for an empty key, KeyTooLarge or
    /// ValueTooLarge when the size limits in types.hpp are exceeded.
    Result set(std::string_view key, std::string_view value);

    /// Returns the stored value, or NotFound if the key is absent.
    [[nodiscard]] Result get(std::string_view key) const;

    /// Removes `key`. Returns NotFound if it was not present, so that the
    /// caller can tell a deletion from a no-op.
    Result remove(std::string_view key);

    /// True if `key` is present. An invalid key is simply absent, so this
    /// reports false rather than a distinct error.
    [[nodiscard]] bool exists(std::string_view key) const;

    /// All keys currently stored, in unspecified order.
    /// Callers that need a stable order sort the result themselves; leaving
    /// that to them keeps this O(n) rather than O(n log n).
    [[nodiscard]] std::vector<Key> keys() const;

    /// Removes every entry.
    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

private:
    /// Transparent hasher.
    ///
    /// Declaring is_transparent opts into C++20 heterogeneous lookup, which
    /// lets find() accept a std::string_view directly. Without it, every
    /// lookup would construct a temporary std::string -- an allocation on the
    /// hot path of a database whose whole job is lookups.
    struct StringHash {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(std::string_view key) const noexcept {
            return std::hash<std::string_view>{}(key);
        }
    };

    /// std::equal_to<> (the void specialisation) is likewise transparent, so
    /// comparisons happen without materialising a std::string either.
    std::unordered_map<Key, Value, StringHash, std::equal_to<>> entries_;
};

}  // namespace minidb

#endif  // MINIDB_DATABASE_HPP
