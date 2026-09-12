#ifndef MINIDB_WAL_HPP
#define MINIDB_WAL_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "minidb/result.hpp"
#include "minidb/types.hpp"

namespace minidb {

/// What a log record asks the database to do when it is replayed.
///
/// CLEAR exists because without it recovery would be wrong: if a session
/// emptied the database and then crashed, replaying only the SETs would
/// resurrect every key the user deleted. Logging the clear as one record
/// keeps it atomic, where logging a DELETE per key could half-apply.
enum class WalOperation : std::uint16_t {
    Set = 1,
    Delete = 2,
    Clear = 3,
};

/// One decoded log record. `key` and `value` are empty where the operation
/// does not use them: Delete has no value, Clear has neither.
struct WalRecord {
    WalOperation operation;
    std::uint64_t sequence;
    Key key;
    Value value;
};

/// MiniDB's write-ahead log.
///
/// The snapshot in StorageManager is only written when a session ends, so a
/// process that dies mid-session loses everything it did. The log closes that
/// window: every mutation is appended here and flushed *before* it is applied
/// in memory, so a recovery pass can rebuild the work that the snapshot does
/// not yet contain.
///
/// The relationship between the two files is deliberately simple:
///
///   snapshot  = the database as of the last successful save
///   log       = every mutation applied since that save, in order
///   recovered = snapshot, then the log replayed on top
///
/// The log is a plain append-only file. Records are self-describing, so a
/// damaged one can be identified without consulting anything else, and the
/// format is documented byte by byte in docs/WAL.md.
///
/// Complexity, for n records:
///
///   append   O(key bytes + value bytes) plus open, write, flush, close
///   replay   O(n) time, O(n) memory for the records produced
///   reset    O(1)
///
/// Durability: append() flushes to the operating system, which means a
/// record is intended to survive process termination with a healthy OS. It is not guaranteed to
/// survive the machine losing power, because no device-level flush is
/// performed. docs/WAL.md states this precisely; it is the difference between
/// surviving a crash and surviving a power cut, and MiniDB only claims the
/// first.
class WriteAheadLog {
public:
    /// Starts each record, so a torn or foreign record is recognised without
    /// relying on position in the file.
    static constexpr char kMagic[4] = {'M', 'W', 'A', 'L'};

    /// Bumped when the record layout changes incompatibly. A reader that
    /// meets an unknown version refuses the log rather than guessing.
    static constexpr std::uint16_t kFormatVersion = 1;

    /// magic(4) + version(2) + operation(2) + sequence(8) + key_length(4)
    /// + value_length(4) + record_crc32(4)
    static constexpr std::size_t kRecordHeaderSize = 28;

    /// The first sequence number when no snapshot checkpoint is supplied.
    static constexpr std::uint64_t kFirstSequence = 1;

    explicit WriteAheadLog(std::filesystem::path log_path);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /// True when a regular file exists at the log path.
    [[nodiscard]] bool exists() const;

    /// Highest recovered or checkpointed sequence, including after reset.
    [[nodiscard]] std::uint64_t last_sequence() const noexcept { return last_sequence_; }

    /// Appends a record and flushes it.
    ///
    /// Precondition: the tail of the log must be known, which means replay()
    /// or reset() has been called since construction, unless the log did not
    /// exist. Appending without that would restart sequence numbering and
    /// leave duplicates in the file, so it is refused rather than allowed to
    /// corrupt the log.
    Result append_set(std::string_view key, std::string_view value);
    Result append_delete(std::string_view key);
    Result append_clear();

    /// Validates every complete record but returns only sequences newer than
    /// checkpoint. Clears records first and establishes the tail for appends.
    /// Pass the matching snapshot checkpoint, including for an empty WAL.
    ///
    /// Returns NotFound when no log file exists -- normal for a database that
    /// has never been written to, and not an error. A log that is damaged
    /// in any complete record (including the final one) is refused with CorruptData
    /// and `records` is left empty: a corrupt log is never quietly treated as
    /// an empty one.
    ///
    /// A truncated final record is the expected result of a crash partway
    /// through an append. Every complete record before it is replayed, the
    /// fragment is discarded, and the file is trimmed back to the last record
    /// boundary so the next append lands in a consistent place.
    Result replay(std::vector<WalRecord>& records, std::uint64_t checkpoint = 0);

    /// Number of bytes discarded by the last replay() because they formed an
    /// incomplete trailing record. Zero when the log ended cleanly.
    [[nodiscard]] std::uint64_t discarded_tail_bytes() const noexcept {
        return discarded_tail_bytes_;
    }

    /// Empties the log; the next append uses checkpoint + 1.
    /// Database passes the installed checkpoint. Default zero is for standalone logs.
    ///
    /// Only safe to call once a snapshot containing every logged mutation has
    /// been installed. Database::save enforces that ordering.
    Result reset(std::uint64_t checkpoint = 0);

private:
    /// Shared by the three append_* entry points.
    Result append(WalOperation operation, std::string_view key, std::string_view value);

    std::filesystem::path path_;

    std::uint64_t last_sequence_ = 0;
    std::uint64_t discarded_tail_bytes_ = 0;

    /// False until replay() or reset() has established where the log ends.
    bool tail_known_ = false;
};

}  // namespace minidb

#endif  // MINIDB_WAL_HPP
