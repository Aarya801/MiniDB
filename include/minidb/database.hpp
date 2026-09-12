#ifndef MINIDB_DATABASE_HPP
#define MINIDB_DATABASE_HPP

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "minidb/hash_table.hpp"
#include "minidb/result.hpp"
#include "minidb/storage.hpp"
#include "minidb/types.hpp"

namespace minidb {

/// An in-memory key-value store.
///
/// Entries live in MiniDB's own HashTable. A Database may additionally be
/// bound to a snapshot file, in which case load() reads it and save() writes
/// it; a default-constructed Database is purely in memory and touches no
/// disk at all.
///
/// Coordination is all this class does. The hash table holds the data, the
/// StorageManager turns records into bytes, and the CLI decides when to load
/// and save. Database owns no serialisation logic and exposes none: callers
/// cannot reach the file format through it.
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
    /// An in-memory database. Nothing is read from or written to disk.
    Database() = default;

    /// A database bound to a snapshot file.
    ///
    /// Construction performs no I/O, so it cannot fail and needs no
    /// exceptions. Call load() to read an existing snapshot.
    explicit Database(std::filesystem::path snapshot_path);

    /// True when this database is bound to a snapshot file.
    [[nodiscard]] bool is_persistent() const noexcept { return storage_.has_value(); }

    /// The snapshot path. Precondition: is_persistent().
    [[nodiscard]] const std::filesystem::path& snapshot_path() const;

    /// True when a snapshot file is already present, so a caller can tell a
    /// first run from a reopened database. False when not persistent.
    [[nodiscard]] bool snapshot_exists() const;

    /// Replaces the contents of the database with the snapshot's.
    ///
    /// A missing snapshot is not a failure: the database is simply left
    /// empty, which is what a first run should do. Any other failure -- a
    /// damaged file, an unsupported version, an unreadable path -- is
    /// returned, and the database is left exactly as it was. A corrupt
    /// snapshot never turns into an empty database.
    ///
    /// O(n) in the number of records stored.
    Result load();

    /// Writes the entire database to the snapshot, replacing it.
    ///
    /// O(n) in the number of entries, in both time and the memory used to
    /// collect them. Snapshots are whole-file: there is no partial save.
    Result save() const;

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

    using EntryTable = HashTable<Key, Value, StringHash>;

    /// HashTable compares with ==, and std::string == std::string_view
    /// already works, so the hasher above is all that is needed to keep
    /// lookups allocation-free.
    EntryTable entries_;

    /// Absent for an in-memory database. An optional rather than a
    /// StorageManager with an empty path, so "not persistent" is a state the
    /// type can express instead of one every method has to check for.
    std::optional<StorageManager> storage_;
};

}  // namespace minidb

#endif  // MINIDB_DATABASE_HPP
