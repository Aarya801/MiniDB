#ifndef MINIDB_STORAGE_HPP
#define MINIDB_STORAGE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "minidb/result.hpp"
#include "minidb/types.hpp"

namespace minidb {

/// One key/value pair, as it appears in a snapshot.
///
/// StorageManager deals in these rather than in HashTable or Database types,
/// so the serialisation code knows nothing about how entries are stored in
/// memory and the database knows nothing about bytes on disk.
struct Record {
    Key key;
    Value value;
};

/// Reads and writes MiniDB's on-disk snapshot.
///
/// A snapshot is the whole database written out in one file. There is no
/// incremental update: saving rewrites everything, loading reads everything.
/// That is the honest limitation of snapshot-only persistence, and it is what
/// the write-ahead log in Milestone 4 exists to address.
///
/// The file layout is specified byte by byte in docs/STORAGE_FORMAT.md.
///
/// Complexity, for n records:
///
///   save    O(n) time, O(1) extra memory beyond the records handed in
///   load    O(n) time, O(n) memory for the records produced
///
/// Nothing about persistence is O(1). Both operations touch every record.
///
/// Every byte read back is treated as untrusted input. A file MiniDB did not
/// write -- or one that was truncated, edited, or damaged -- must be rejected
/// rather than turned into records, and must never be allowed to drive an
/// allocation or an index.
class StorageManager {
public:
    /// Written at the start of every snapshot so a file that is not a MiniDB
    /// snapshot is rejected immediately instead of being parsed as one.
    static constexpr char kMagic[8] = {'M', 'I', 'N', 'I', 'D', 'B', 'S', 'S'};

    /// Legacy writer version. A reader that meets a
    /// version it does not know refuses the file instead of guessing.
    static constexpr std::uint32_t kFormatVersion = 1;

    /// magic(8) + version(4) + record_count(8) + payload_crc32(4)
    static constexpr std::size_t kHeaderSize = 24;
    static constexpr std::uint32_t kCheckpointVersion = 2;
    static constexpr std::size_t kCheckpointHeaderSize = 32;

    explicit StorageManager(std::filesystem::path snapshot_path);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /// The scratch file that save() writes before replacing the snapshot.
    [[nodiscard]] std::filesystem::path temporary_path() const;

    /// True when a regular file exists at the snapshot path. A directory or a
    /// special file at that path is not a snapshot and reports false, so that
    /// load() reports the problem rather than this being mistaken for "new
    /// database".
    [[nodiscard]] bool snapshot_exists() const;

    /// Writes every record, replacing any existing snapshot.
    /// An explicit checkpoint selects version 2; omission retains version 1.
    ///
    /// The records go to a temporary file which then replaces the snapshot in
    /// one filesystem operation, so a reader never observes a half-written
    /// file. See docs/STORAGE_FORMAT.md for what this does and does not
    /// guarantee against power loss.
    Result save(const std::vector<Record>& records,
                std::optional<std::uint64_t> checkpoint = std::nullopt) const;

    /// Reads version 1 or 2 into `records`, which is cleared first.
    /// If supplied, checkpoint receives the validated sequence (zero for v1).
    /// On failure it is zero; no unvalidated checkpoint is returned.
    ///
    /// Returns NotFound when no snapshot exists, which callers normally treat
    /// as "start empty" rather than as a failure. Any other failure means the
    /// file is unusable and `records` is left empty -- a damaged snapshot is
    /// never quietly reported as an empty database.
    Result load(std::vector<Record>& records, std::uint64_t* checkpoint = nullptr) const;

private:
    std::filesystem::path path_;
};

/// Where the CLI keeps its database when no path is given.
///
/// Resolved from the environment so no machine-specific path is compiled in,
/// and deliberately outside the source tree so running MiniDB never writes
/// into the repository:
///
///   Windows  %LOCALAPPDATA%\MiniDB\minidb.snapshot
///   other    $XDG_DATA_HOME/minidb/minidb.snapshot
///            or $HOME/.local/share/minidb/minidb.snapshot
///
/// Falls back to the current directory if the environment says nothing, which
/// is the only case where MiniDB writes where it was started from.
[[nodiscard]] std::filesystem::path default_snapshot_path();

}  // namespace minidb

#endif  // MINIDB_STORAGE_HPP
