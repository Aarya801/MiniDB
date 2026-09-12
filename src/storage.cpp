#include "minidb/storage.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <string>
#include <utility>

#include "minidb/hash_table.hpp"

namespace minidb {
namespace {

namespace fs = std::filesystem;

/// Offsets of the header fields. Named rather than spelled inline so the
/// reader, the writer and docs/STORAGE_FORMAT.md cannot drift apart.
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kVersionOffset = 8;
constexpr std::size_t kRecordCountOffset = 12;
constexpr std::size_t kChecksumOffset = 20;

/// Bytes every record occupies even when it is as small as possible:
/// two length fields plus at least one byte of key. Used to reject a record
/// count that could not possibly fit in the file.
constexpr std::uint64_t kMinRecordBytes = 9;

// -------------------------------------------------------------------------
// Little-endian encoding
//
// Integers are written byte by byte, least significant first, rather than by
// copying the bytes of the in-memory value. That makes a snapshot written on
// one machine readable on another regardless of the CPU's byte order, and it
// is why there is no reinterpret_cast anywhere in this file.
// -------------------------------------------------------------------------

void append_u32(std::string& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFFU));
    }
}

void append_u64(std::string& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFFU));
    }
}

[[nodiscard]] std::uint32_t read_u32(const char* bytes) noexcept {
    std::uint32_t value = 0;
    for (int index = 3; index >= 0; --index) {
        value <<= 8;
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index]));
    }
    return value;
}

[[nodiscard]] std::uint64_t read_u64(const char* bytes) noexcept {
    std::uint64_t value = 0;
    for (int index = 7; index >= 0; --index) {
        value <<= 8;
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[index]));
    }
    return value;
}

// -------------------------------------------------------------------------
// CRC-32
//
// The standard CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320) over
// every payload byte. Computed without a lookup table: eight shifts per byte
// is more than fast enough next to the cost of the disk I/O, and it keeps the
// implementation short enough to read.
//
// What it buys: the magic number catches a file that is not a snapshot, and
// the length checks catch a structurally broken one, but neither notices a
// single flipped bit inside a key or a value. The checksum does.
// -------------------------------------------------------------------------

constexpr std::uint32_t kCrcInitial = 0xFFFFFFFFU;

[[nodiscard]] std::uint32_t crc32_update(std::uint32_t crc, const char* data,
                                         std::size_t length) noexcept {
    for (std::size_t index = 0; index < length; ++index) {
        crc ^= static_cast<std::uint32_t>(static_cast<unsigned char>(data[index]));
        for (int bit = 0; bit < 8; ++bit) {
            crc = ((crc & 1U) != 0U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
        }
    }
    return crc;
}

[[nodiscard]] std::uint32_t crc32_update(std::uint32_t crc, const std::string& data) noexcept {
    return crc32_update(crc, data.data(), data.size());
}

[[nodiscard]] std::uint32_t crc32_finish(std::uint32_t crc) noexcept {
    return crc ^ 0xFFFFFFFFU;
}

/// Removes a file when destroyed, unless it has been kept.
///
/// Every failure path in save() -- and any exception thrown along the way --
/// must leave no scratch file behind. Doing that with an RAII guard rather
/// than a remove() call at each early return means a path added later cannot
/// forget to clean up.
class ScratchFile {
public:
    explicit ScratchFile(fs::path path) : path_(std::move(path)) {}

    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;
    ScratchFile(ScratchFile&&) = delete;
    ScratchFile& operator=(ScratchFile&&) = delete;

    ~ScratchFile() {
        if (remove_on_destruction_) {
            std::error_code ignored;
            fs::remove(path_, ignored);
        }
    }

    /// Called once the file has become the real snapshot, so it is not
    /// deleted from under its new name.
    void keep() noexcept { remove_on_destruction_ = false; }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
    bool remove_on_destruction_ = true;
};

/// Reads exactly `length` bytes, or reports that the file ended early.
[[nodiscard]] bool read_exact(std::istream& in, char* into, std::size_t length) {
    in.read(into, static_cast<std::streamsize>(length));
    return in.good() && static_cast<std::size_t>(in.gcount()) == length;
}

[[nodiscard]] Result truncated(const char* what) {
    return Result::failure(StatusCode::CorruptData,
                           std::string("snapshot ends unexpectedly while reading ") + what);
}

/// Reads a value already validated to fit within the file, into a string.
///
/// `length` has been checked against the size limits and against the bytes
/// actually left in the file before this is called. That ordering is the
/// whole point: resize() is never handed a number that came straight off
/// disk.
[[nodiscard]] bool read_bytes_into(std::istream& in, std::string& into, std::uint32_t length) {
    into.resize(length);
    if (length == 0) {
        return true;
    }
    return read_exact(in, into.data(), length);
}

/// Reads an environment variable, treating an empty value as unset.
[[nodiscard]] const char* environment_value(const char* name) noexcept {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return nullptr;
    }
    return value;
}

}  // namespace

StorageManager::StorageManager(fs::path snapshot_path) : path_(std::move(snapshot_path)) {}

fs::path StorageManager::temporary_path() const {
    // Sits beside the snapshot on purpose. Replacing a file is only atomic
    // within one filesystem, so the scratch file must not live in the system
    // temporary directory, which is often a different volume.
    fs::path temporary = path_;
    temporary += ".tmp";
    return temporary;
}

bool StorageManager::snapshot_exists() const {
    std::error_code ignored;
    return fs::is_regular_file(path_, ignored);
}

Result StorageManager::save(const std::vector<Record>& records) const {
    if (records.size() > limits::kMaxRecordCount) {
        return Result::failure(StatusCode::InvalidArgument,
                               "too many records to store in one snapshot");
    }

    // Refuse to write a file that load() would reject. Without this a value
    // that slipped past the database's own checks would produce a snapshot
    // that cannot be read back -- a far worse failure than declining to save.
    for (const Record& record : records) {
        if (record.key.empty() || record.key.size() > limits::kMaxKeySize) {
            return Result::failure(StatusCode::KeyTooLarge,
                                   "record key is empty or exceeds the key size limit");
        }
        if (record.value.size() > limits::kMaxValueSize) {
            return Result::failure(StatusCode::ValueTooLarge,
                                   "record value exceeds the value size limit");
        }
    }

    std::error_code error;
    const fs::path parent = path_.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, error);
        if (error && !fs::is_directory(parent)) {
            return Result::failure(StatusCode::IoError,
                                   "cannot create database directory: " + error.message());
        }
    }

    ScratchFile scratch(temporary_path());
    {
        std::ofstream out(scratch.path(), std::ios::binary | std::ios::trunc);
        if (!out) {
            return Result::failure(StatusCode::IoError,
                                   "cannot create temporary snapshot beside the database file");
        }

        // The header goes out with a zero checksum, because the real value is
        // only known once the payload has been written. It is patched in at
        // the end by seeking back, which avoids either encoding every record
        // twice or holding the whole encoded payload in memory.
        std::string header;
        header.reserve(kHeaderSize);
        header.append(kMagic, sizeof(kMagic));
        append_u32(header, kFormatVersion);
        append_u64(header, static_cast<std::uint64_t>(records.size()));
        append_u32(header, 0);
        out.write(header.data(), static_cast<std::streamsize>(header.size()));

        std::uint32_t crc = kCrcInitial;
        std::string encoded;
        for (const Record& record : records) {
            encoded.clear();
            append_u32(encoded, static_cast<std::uint32_t>(record.key.size()));
            append_u32(encoded, static_cast<std::uint32_t>(record.value.size()));
            encoded.append(record.key);
            encoded.append(record.value);

            crc = crc32_update(crc, encoded);
            out.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
            if (!out) {
                return Result::failure(StatusCode::IoError,
                                       "failed while writing snapshot records");
            }
        }

        std::string checksum;
        append_u32(checksum, crc32_finish(crc));
        out.seekp(static_cast<std::streamoff>(kChecksumOffset));
        out.write(checksum.data(), static_cast<std::streamsize>(checksum.size()));

        out.flush();
        if (!out) {
            return Result::failure(StatusCode::IoError, "failed while finishing snapshot");
        }
    }  // The stream closes here, before the file is renamed.

    // One filesystem operation makes the new file the snapshot. A reader
    // either sees the whole previous snapshot or the whole new one, never a
    // half-written file.
    fs::rename(scratch.path(), path_, error);
    if (error) {
        return Result::failure(StatusCode::IoError,
                               "cannot replace the database file: " + error.message());
    }

    scratch.keep();
    return Result::ok();
}

Result StorageManager::load(std::vector<Record>& records) const {
    records.clear();

    std::error_code error;
    if (!fs::exists(path_, error)) {
        // Not a failure for the caller: a database that has never been saved
        // simply starts empty.
        return Result::failure(StatusCode::NotFound, "no snapshot at " + path_.string());
    }
    if (!snapshot_exists()) {
        return Result::failure(StatusCode::IoError,
                               "database path exists but is not a regular file");
    }

    const std::uintmax_t file_size = fs::file_size(path_, error);
    if (error) {
        return Result::failure(StatusCode::IoError,
                               "cannot determine snapshot size: " + error.message());
    }
    if (file_size > limits::kMaxSnapshotSize) {
        return Result::failure(StatusCode::CorruptData, "snapshot is larger than the size limit");
    }
    if (file_size < kHeaderSize) {
        return Result::failure(StatusCode::CorruptData, "snapshot is too small to hold a header");
    }

    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return Result::failure(StatusCode::IoError, "cannot open the database file for reading");
    }

    std::array<char, kHeaderSize> header{};
    if (!read_exact(in, header.data(), header.size())) {
        return truncated("the header");
    }

    if (std::memcmp(header.data() + kMagicOffset, kMagic, sizeof(kMagic)) != 0) {
        return Result::failure(StatusCode::CorruptData,
                               "not a MiniDB snapshot (magic number does not match)");
    }

    const std::uint32_t version = read_u32(header.data() + kVersionOffset);
    if (version != kFormatVersion) {
        return Result::failure(StatusCode::UnsupportedVersion,
                               "snapshot format version " + std::to_string(version) +
                                   " is not supported (this build reads version " +
                                   std::to_string(kFormatVersion) + ")");
    }

    const std::uint64_t record_count = read_u64(header.data() + kRecordCountOffset);
    const std::uint32_t stored_checksum = read_u32(header.data() + kChecksumOffset);

    if (record_count > limits::kMaxRecordCount) {
        return Result::failure(StatusCode::CorruptData,
                               "snapshot claims more records than the limit allows");
    }

    // The count is compared against the space actually available before it is
    // trusted for anything. Dividing rather than multiplying keeps the check
    // free of overflow.
    const std::uint64_t payload_bytes = static_cast<std::uint64_t>(file_size) - kHeaderSize;
    if (record_count > payload_bytes / kMinRecordBytes) {
        return Result::failure(StatusCode::CorruptData,
                               "snapshot claims " + std::to_string(record_count) +
                                   " records, more than its size can hold");
    }

    // Records are parsed into a local vector and handed over only once the
    // whole file has checked out. Pushing straight into the caller's vector
    // would leave it holding a partial snapshot on every failure path below,
    // and "half a database" is worse than none.
    std::vector<Record> parsed;

    // Safe now: the count has been bounded twice over.
    parsed.reserve(static_cast<std::size_t>(record_count));

    // Keys already seen, so a file listing the same key twice is rejected
    // rather than silently collapsing to whichever record happens to be last.
    HashTable<Key, bool> seen(static_cast<std::size_t>(record_count) + 1);

    std::uint32_t crc = kCrcInitial;
    std::uint64_t consumed = 0;

    for (std::uint64_t index = 0; index < record_count; ++index) {
        std::array<char, 8> lengths{};
        if (!read_exact(in, lengths.data(), lengths.size())) {
            return truncated("a record header");
        }
        crc = crc32_update(crc, lengths.data(), lengths.size());
        consumed += lengths.size();

        const std::uint32_t key_length = read_u32(lengths.data());
        const std::uint32_t value_length = read_u32(lengths.data() + 4);

        if (key_length == 0 || key_length > limits::kMaxKeySize) {
            return Result::failure(StatusCode::CorruptData,
                                   "record " + std::to_string(index) +
                                       " declares an invalid key length of " +
                                       std::to_string(key_length));
        }
        if (value_length > limits::kMaxValueSize) {
            return Result::failure(StatusCode::CorruptData,
                                   "record " + std::to_string(index) +
                                       " declares an invalid value length of " +
                                       std::to_string(value_length));
        }

        // Both operands are 32-bit and the sum is computed as 64-bit, so this
        // cannot wrap. The comparison then proves the bytes are really there
        // before either string is resized.
        const std::uint64_t declared =
            static_cast<std::uint64_t>(key_length) + static_cast<std::uint64_t>(value_length);
        if (declared > payload_bytes - consumed) {
            return Result::failure(StatusCode::CorruptData,
                                   "record " + std::to_string(index) +
                                       " declares more bytes than the snapshot contains");
        }

        Record record;
        if (!read_bytes_into(in, record.key, key_length)) {
            return truncated("a record key");
        }
        if (!read_bytes_into(in, record.value, value_length)) {
            return truncated("a record value");
        }
        crc = crc32_update(crc, record.key);
        crc = crc32_update(crc, record.value);
        consumed += declared;

        if (!seen.insert_or_assign(record.key, true)) {
            return Result::failure(StatusCode::CorruptData,
                                   "snapshot contains the key '" + record.key + "' twice");
        }
        parsed.push_back(std::move(record));
    }

    // Anything after the last record means the file is not what its header
    // describes, so it is not trustworthy even though the records parsed.
    if (consumed != payload_bytes) {
        return Result::failure(StatusCode::CorruptData,
                               "snapshot has " + std::to_string(payload_bytes - consumed) +
                                   " unexpected trailing bytes");
    }

    if (crc32_finish(crc) != stored_checksum) {
        return Result::failure(StatusCode::CorruptData,
                               "snapshot checksum does not match its contents");
    }

    records = std::move(parsed);
    return Result::ok();
}

fs::path default_snapshot_path() {
    constexpr const char* kDirectoryName = "MiniDB";
    constexpr const char* kFileName = "minidb.snapshot";

#if defined(_WIN32)
    if (const char* local_app_data = environment_value("LOCALAPPDATA")) {
        return fs::path(local_app_data) / kDirectoryName / kFileName;
    }
#else
    if (const char* data_home = environment_value("XDG_DATA_HOME")) {
        return fs::path(data_home) / "minidb" / kFileName;
    }
    if (const char* home = environment_value("HOME")) {
        return fs::path(home) / ".local" / "share" / "minidb" / kFileName;
    }
#endif

    // Nothing in the environment to go on. Staying in the current directory
    // is better than guessing at an absolute path that may not be writable.
    return fs::path(kFileName);
}

}  // namespace minidb
