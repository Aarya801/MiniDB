#ifndef MINIDB_DATABASE_HPP
#define MINIDB_DATABASE_HPP

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "minidb/hash_table.hpp"
#include "minidb/lru_cache.hpp"
#include "minidb/result.hpp"
#include "minidb/storage.hpp"
#include "minidb/types.hpp"
#include "minidb/wal.hpp"

namespace minidb {

class Transaction;
struct TransactionMutation;

/// An in-memory key-value store.
///
/// Entries live in MiniDB's own HashTable. A Database may additionally be
/// bound to a snapshot file, in which case load() reads it and save() writes
/// it; a default-constructed Database is purely in memory and touches no
/// disk at all.
///
/// Coordination is all this class does. The hash table holds the data, the
/// StorageManager turns records into bytes, the WriteAheadLog records
/// mutations as they happen, and the CLI decides when to load and save.
/// Database owns no serialisation logic and exposes none: callers cannot
/// reach either file format through it.
///
/// A persistent database keeps two files:
///
///   <path>       the snapshot: the database as of the last successful save
///   <path>.wal   the log: every mutation applied since that save
///
/// Every mutation is written to the log and flushed *before* it changes
/// memory. A failed append may still be present during recovery. load()
/// rebuilds state as snapshot-then-log; save() writes a new snapshot and only
/// then resets the log.
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
    static constexpr std::size_t kDefaultCacheCapacity = 128;
    Database() = default;
    explicit Database(std::size_t cache_capacity) : cache_(cache_capacity) {}

    /// A database bound to a snapshot file.
    ///
    /// Construction inspects WAL metadata but reads no records.
    /// Call load() before reads. The first mutation/save recovers automatically.
    explicit Database(std::filesystem::path snapshot_path,
                      std::size_t cache_capacity = kDefaultCacheCapacity);

    /// True when this database is bound to a snapshot file.
    [[nodiscard]] bool is_persistent() const noexcept { return storage_.has_value(); }

    /// The snapshot path. Precondition: is_persistent().
    [[nodiscard]] const std::filesystem::path& snapshot_path() const;

    /// True when a snapshot file is already present, so a caller can tell a
    /// first run from a reopened database. False when not persistent.
    [[nodiscard]] bool snapshot_exists() const;

    /// The write-ahead log path. Precondition: is_persistent().
    [[nodiscard]] const std::filesystem::path& wal_path() const;

    /// Recovers the database: loads the snapshot, then replays newer WAL records.
    /// Validates but skips sequences already represented by its checkpoint.
    ///
    /// A missing snapshot and a missing log are both fine -- together they
    /// mean a first run, and the database is left empty. Any real failure --
    /// a damaged file, an unsupported version, an unreadable path -- is
    /// returned, and the database is left exactly as it was. Neither a
    /// corrupt snapshot nor a corrupt log ever turns into an empty database.
    ///
    /// Replay applies records directly to the table, never through set() or
    /// remove(), so recovering does not write new log records.
    ///
    /// O(n + m) for n snapshot records and m logged operations.
    Result load();

    /// Opens this persistent database by recovering snapshot then WAL.
    Result open() { return load(); }

    /// Number of logged operations replayed by the last load(). Lets the CLI
    /// tell the user that work was recovered.
    [[nodiscard]] std::size_t replayed_operation_count() const noexcept {
        return replayed_operation_count_;
    }

    /// Writes the entire database to the snapshot, then resets the log.
    ///
    /// The order matters and is not negotiable: the log is only cleared once
    /// the snapshot that supersedes it is safely in place. If the snapshot
    /// fails, the log is left untouched and every logged operation is still
    /// recoverable.
    ///
    /// O(n) in the number of entries, in both time and the memory used to
    /// collect them. Snapshots are whole-file: there is no partial save.
    Result save();

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
    ///
    /// Returns a Result because on a persistent database this is logged
    /// first, and logging can fail. A clear that was not logged would be
    /// undone by recovery: replaying the earlier SETs would bring every key
    /// back.
    Result clear();

    /// Cache diagnostics; capacity counts entries, not bytes. Zero disables it.
    [[nodiscard]] std::size_t cache_size() const noexcept { return cache_.size(); }
    [[nodiscard]] std::size_t cache_capacity() const noexcept { return cache_.capacity(); }

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

private:
    friend class Transaction;
    Result commit_transaction(const std::vector<TransactionMutation>& mutations);
    Result ensure_loaded();
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

    /// Applies one recovered log record straight to a table.
    ///
    /// Deliberately not routed through set()/remove()/clear(): those append to
    /// the log, and replay must not write the records it is reading.
    static void apply_recovered(EntryTable& table, const WalRecord& record);

    /// HashTable compares with ==, and std::string == std::string_view
    /// already works, so the hasher above is all that is needed to keep
    /// lookups allocation-free.
    EntryTable entries_;
    // Logical constness: GET changes cache recency, never authoritative data.
    mutable LruCache cache_{kDefaultCacheCapacity};

    /// Both absent for an in-memory database, both present for a persistent
    /// one. Optionals rather than objects with empty paths, so "not
    /// persistent" is a state the types can express instead of one every
    /// method has to check for.
    std::optional<StorageManager> storage_;
    std::optional<WriteAheadLog> wal_;

    std::size_t replayed_operation_count_ = 0;
    bool loaded_ = false;
};

}  // namespace minidb

#endif  // MINIDB_DATABASE_HPP
