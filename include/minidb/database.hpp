#ifndef MINIDB_DATABASE_HPP
#define MINIDB_DATABASE_HPP

#include <cstddef>
#include <functional>
#include <string_view>
#include <vector>

#include "minidb/hash_table.hpp"
#include "minidb/result.hpp"
#include "minidb/types.hpp"

namespace minidb {

/// An in-memory key-value store.
///
/// Entries live in MiniDB's own HashTable and are lost when the process
/// exits; persistence arrives in Milestone 3. Milestone 1 used a
/// std::unordered_map behind this same interface, and swapping it for the
/// hand-written table changed one private member and three lines of
/// database.cpp -- nothing in the CLI or the database tests moved.
///
/// The class knows nothing about terminals or command syntax. Callers hand it
/// keys and values and receive a Result; formatting replies is the CLI's job.
///
/// Complexity, as provided by HashTable:
///
///   set     average O(1), worst case O(n)
///   get     average O(1), worst case O(n)
///   remove  average O(1), worst case O(n)
///   exists  average O(1), worst case O(n)
///   keys    O(n)
///   clear   O(n)
///   size    O(1)
///
/// The averages are expected values, not guarantees. They hold while the
/// hash spreads keys evenly and the load factor stays bounded, which the
/// table enforces by rehashing at 0.75. A lookup degrades toward O(n) when
/// many keys collide into one bucket, which unlucky or adversarial key sets
/// can still cause.
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
    /// Hashes a std::string_view, so HashTable can look up a key without
    /// first constructing a std::string. Without it every get, exists and
    /// remove would allocate a temporary string purely to throw it away --
    /// an allocation on the hot path of a database whose job is lookups.
    ///
    /// std::hash<std::string_view> and std::hash<std::string> are required to
    /// produce the same value for the same characters, so a view and the
    /// stored string agree on their bucket.
    struct StringHash {
        [[nodiscard]] std::size_t operator()(std::string_view key) const noexcept {
            return std::hash<std::string_view>{}(key);
        }
    };

    /// HashTable compares with ==, and std::string == std::string_view
    /// already works, so the hasher above is all that is needed to keep
    /// lookups allocation-free.
    HashTable<Key, Value, StringHash> entries_;
};

}  // namespace minidb

#endif  // MINIDB_DATABASE_HPP
