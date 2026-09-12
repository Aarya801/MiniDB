#include "minidb/wal.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <ios>
#include <limits>
#include <string>
#include <utility>

#include "binary_format.hpp"

namespace minidb {
namespace {

namespace fs = std::filesystem;

/// Field offsets within a record header. Named so the reader, the writer and
/// docs/WAL.md cannot drift apart.
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kOperationOffset = 6;
constexpr std::size_t kSequenceOffset = 8;
constexpr std::size_t kKeyLengthOffset = 16;
constexpr std::size_t kValueLengthOffset = 20;
constexpr std::size_t kChecksumOffset = 24;

/// Bytes of the header that the checksum covers: everything up to, but not
/// including, the checksum field itself.
constexpr std::size_t kChecksumedHeaderBytes = kChecksumOffset;

[[nodiscard]] bool is_known_operation(std::uint16_t value) noexcept {
    return value == static_cast<std::uint16_t>(WalOperation::Set) ||
           value == static_cast<std::uint16_t>(WalOperation::Delete) ||
           value == static_cast<std::uint16_t>(WalOperation::Clear);
}

[[nodiscard]] const char* operation_name(WalOperation operation) noexcept {
    switch (operation) {
        case WalOperation::Set:
            return "SET";
        case WalOperation::Delete:
            return "DELETE";
        case WalOperation::Clear:
            return "CLEAR";
    }
    return "UNKNOWN";
}

/// Checks that the key and value make sense for the operation.
///
/// Applied on the way in as well as on the way out: the log must never
/// contain a record that replay would reject, because that would turn a
/// recoverable database into an unreadable one.
[[nodiscard]] Result validate_fields(WalOperation operation, std::size_t key_length,
                                     std::size_t value_length) {
    switch (operation) {
        case WalOperation::Set:
            if (key_length == 0 || key_length > limits::kMaxKeySize) {
                return Result::failure(StatusCode::KeyTooLarge,
                                       "SET record key is empty or exceeds the key size limit");
            }
            if (value_length > limits::kMaxValueSize) {
                return Result::failure(StatusCode::ValueTooLarge,
                                       "SET record value exceeds the value size limit");
            }
            return Result::ok();

        case WalOperation::Delete:
            if (key_length == 0 || key_length > limits::kMaxKeySize) {
                return Result::failure(StatusCode::KeyTooLarge,
                                       "DELETE record key is empty or exceeds the key size limit");
            }
            if (value_length != 0) {
                return Result::failure(StatusCode::CorruptData,
                                       "DELETE record carries a value, which it must not");
            }
            return Result::ok();

        case WalOperation::Clear:
            if (key_length != 0 || value_length != 0) {
                return Result::failure(StatusCode::CorruptData,
                                       "CLEAR record carries a key or value, which it must not");
            }
            return Result::ok();
    }
    return Result::failure(StatusCode::CorruptData, "unknown log operation");
}

/// Builds the exact bytes of one record, checksum included.
[[nodiscard]] std::string encode_record(WalOperation operation, std::uint64_t sequence,
                                        std::string_view key, std::string_view value) {
    std::string header;
    header.reserve(WriteAheadLog::kRecordHeaderSize);
    header.append(WriteAheadLog::kMagic, sizeof(WriteAheadLog::kMagic));
    detail::append_u16(header, WriteAheadLog::kFormatVersion);
    detail::append_u16(header, static_cast<std::uint16_t>(operation));
    detail::append_u64(header, sequence);
    detail::append_u32(header, static_cast<std::uint32_t>(key.size()));
    detail::append_u32(header, static_cast<std::uint32_t>(value.size()));

    // The checksum covers the header fields written so far plus the payload,
    // so a flipped bit anywhere in the record is caught -- including one in a
    // length field, which no other check would notice.
    std::uint32_t crc = detail::kCrcInitial;
    crc = detail::crc32_update(crc, header);
    crc = detail::crc32_update(crc, key);
    crc = detail::crc32_update(crc, value);

    detail::append_u32(header, detail::crc32_finish(crc));
    header.append(key);
    header.append(value);
    return header;
}

}  // namespace

WriteAheadLog::WriteAheadLog(fs::path log_path) : path_(std::move(log_path)) {
    // A log with no records has a known tail whether or not the file is
    // there: sequence numbering starts from the beginning either way. That
    // covers both a brand-new database and one that was saved cleanly, since
    // reset() truncates the file rather than deleting it. Only a log that
    // still holds records has to be replayed before it can be appended to.
    std::error_code ignored;
    const bool present = fs::exists(path_, ignored);
    if (!ignored) {
        tail_known_ = !present || (fs::file_size(path_, ignored) == 0 && !ignored);
    }
}

bool WriteAheadLog::exists() const {
    std::error_code ignored;
    return fs::is_regular_file(path_, ignored);
}

Result WriteAheadLog::append_set(std::string_view key, std::string_view value) {
    return append(WalOperation::Set, key, value);
}

Result WriteAheadLog::append_delete(std::string_view key) {
    return append(WalOperation::Delete, key, {});
}

Result WriteAheadLog::append_clear() {
    return append(WalOperation::Clear, {}, {});
}

Result WriteAheadLog::append(WalOperation operation, std::string_view key, std::string_view value) {
    if (!tail_known_) {
        // Appending now would reuse sequence numbers already in the file and
        // make the log unreplayable. Refusing is the safe answer.
        return Result::failure(StatusCode::Internal,
                               "cannot append to a log whose tail is unknown; replay or reset it "
                               "first");
    }

    Result fields = validate_fields(operation, key.size(), value.size());
    if (!fields.is_ok()) {
        return fields;
    }

    if (last_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return Result::failure(StatusCode::Internal, "log sequence exhausted; save a snapshot");
    }
    const std::uint64_t sequence = last_sequence_ + 1;
    const std::string record = encode_record(operation, sequence, key, value);

    std::error_code error;
    const fs::path parent = path_.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, error);
        if (error) {
            return Result::failure(StatusCode::IoError,
                                   "cannot create the directory for the log: " + error.message());
        }
    }

    const bool present = fs::exists(path_, error);
    if (error) {
        return Result::failure(StatusCode::IoError, "cannot inspect the log: " + error.message());
    }
    const std::uintmax_t current_size = present ? fs::file_size(path_, error) : 0;
    if (error) {
        return Result::failure(StatusCode::IoError, "cannot determine the log size");
    }
    if (current_size > limits::kMaxWalSize || record.size() > limits::kMaxWalSize - current_size) {
        return Result::failure(StatusCode::InvalidArgument,
                               "log size limit reached; save a snapshot");
    }

    // Opened per append rather than held open. That costs an open and close
    // per mutation, which is real overhead and deliberately not optimised
    // away: it keeps the log free of lifecycle interactions with replay() and
    // reset(), and keeps this class a plain value. A production log would
    // keep the handle open.
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) {
        return Result::failure(StatusCode::IoError, "cannot open the log for appending");
    }

    // One write for the whole record. The operating system is not obliged to
    // make it atomic, which is exactly why replay tolerates a short final
    // record.
    // Any write/flush/close failure leaves an uncertain tail. Only recovery
    // may establish it again; another append could bury a partial record.
    tail_known_ = false;
    out.write(record.data(), static_cast<std::streamsize>(record.size()));
    out.flush();
    out.close();
    if (!out) {
        return Result::failure(StatusCode::IoError, std::string("failed while appending a ") +
                                                        operation_name(operation) +
                                                        " record to the log");
    }

    // Only now is the record durable enough to count. Advancing the sequence
    // before the write succeeded would leave a gap that replay would reject.
    last_sequence_ = sequence;
    tail_known_ = true;
    return Result::ok();
}

Result WriteAheadLog::replay(std::vector<WalRecord>& records) {
    records.clear();
    discarded_tail_bytes_ = 0;
    tail_known_ = false;

    std::error_code error;
    const bool present = fs::exists(path_, error);
    if (error) {
        return Result::failure(StatusCode::IoError, "cannot inspect the log: " + error.message());
    }
    if (!present) {
        // Nothing logged since the last snapshot. Not a failure: this is the
        // normal state of a database that exited cleanly.
        last_sequence_ = 0;
        tail_known_ = true;
        return Result::failure(StatusCode::NotFound, "no write-ahead log at " + path_.string());
    }
    if (!exists()) {
        return Result::failure(StatusCode::IoError,
                               "the log path exists but is not a regular file");
    }

    const std::uintmax_t file_size = fs::file_size(path_, error);
    if (error) {
        return Result::failure(StatusCode::IoError,
                               "cannot determine the log size: " + error.message());
    }
    if (file_size > limits::kMaxWalSize) {
        return Result::failure(StatusCode::CorruptData, "log is larger than the size limit");
    }

    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return Result::failure(StatusCode::IoError, "cannot open the log for reading");
    }

    // Parsed into a local vector and handed over only on success, so a
    // rejected log never leaves the caller holding half a recovery.
    std::vector<WalRecord> parsed;

    const std::uint64_t total = static_cast<std::uint64_t>(file_size);
    std::uint64_t offset = 0;
    std::uint64_t last_boundary = 0;
    std::uint64_t expected_sequence = kFirstSequence;

    while (offset < total) {
        const std::uint64_t remaining = total - offset;

        // Too few bytes left for even a header: this is the tail of a write
        // that a crash interrupted.
        if (remaining < kRecordHeaderSize) {
            break;
        }

        std::array<char, kRecordHeaderSize> header{};
        in.read(header.data(), static_cast<std::streamsize>(header.size()));
        if (!in.good() || static_cast<std::size_t>(in.gcount()) != header.size()) {
            return Result::failure(StatusCode::IoError, "failed while reading the log header");
        }

        if (std::memcmp(header.data() + kMagicOffset, kMagic, sizeof(kMagic)) != 0) {
            // A complete header that is not a record means damage inside the
            // log, not an interrupted append. Everything after it is
            // unreadable, so the log is refused rather than partly applied.
            return Result::failure(StatusCode::CorruptData,
                                   "log record at byte " + std::to_string(offset) +
                                       " does not start with the expected magic number");
        }

        const std::uint16_t version = detail::read_u16(header.data() + kVersionOffset);
        if (version != kFormatVersion) {
            return Result::failure(StatusCode::UnsupportedVersion,
                                   "log format version " + std::to_string(version) +
                                       " is not supported (this build reads version " +
                                       std::to_string(kFormatVersion) + ")");
        }

        const std::uint16_t raw_operation = detail::read_u16(header.data() + kOperationOffset);
        if (!is_known_operation(raw_operation)) {
            return Result::failure(StatusCode::CorruptData,
                                   "log record at byte " + std::to_string(offset) +
                                       " has unknown operation " + std::to_string(raw_operation));
        }
        const WalOperation operation = static_cast<WalOperation>(raw_operation);

        const std::uint64_t sequence = detail::read_u64(header.data() + kSequenceOffset);
        const std::uint32_t key_length = detail::read_u32(header.data() + kKeyLengthOffset);
        const std::uint32_t value_length = detail::read_u32(header.data() + kValueLengthOffset);
        const std::uint32_t stored_checksum = detail::read_u32(header.data() + kChecksumOffset);

        if (sequence != expected_sequence) {
            return Result::failure(StatusCode::CorruptData, "unexpected log sequence number");
        }

        Result fields = validate_fields(operation, key_length, value_length);
        if (!fields.is_ok()) {
            return Result::failure(
                StatusCode::CorruptData,
                "log record at byte " + std::to_string(offset) + ": " + fields.message());
        }

        // Both lengths are 32-bit and the sum is computed as 64-bit, so it
        // cannot wrap. Comparing it against the bytes actually left proves
        // the payload is there before anything is resized.
        const std::uint64_t payload_bytes =
            static_cast<std::uint64_t>(key_length) + static_cast<std::uint64_t>(value_length);
        if (payload_bytes > remaining - kRecordHeaderSize) {
            // The header promised more than the file holds: an append that
            // was cut short.
            break;
        }

        WalRecord record;
        record.operation = operation;
        record.sequence = sequence;
        record.key.resize(key_length);
        record.value.resize(value_length);

        if (key_length > 0) {
            in.read(record.key.data(), static_cast<std::streamsize>(key_length));
            if (!in.good() || static_cast<std::size_t>(in.gcount()) != key_length) {
                return Result::failure(StatusCode::IoError, "failed while reading the log key");
            }
        }
        if (value_length > 0) {
            in.read(record.value.data(), static_cast<std::streamsize>(value_length));
            if (!in.good() || static_cast<std::size_t>(in.gcount()) != value_length) {
                return Result::failure(StatusCode::IoError, "failed while reading the log value");
            }
        }

        std::uint32_t crc = detail::kCrcInitial;
        crc = detail::crc32_update(crc, header.data(), kChecksumedHeaderBytes);
        crc = detail::crc32_update(crc, record.key);
        crc = detail::crc32_update(crc, record.value);
        if (detail::crc32_finish(crc) != stored_checksum) {
            return Result::failure(
                StatusCode::CorruptData,
                "log record at byte " + std::to_string(offset) + " fails its checksum");
        }

        parsed.push_back(std::move(record));
        offset += kRecordHeaderSize + payload_bytes;
        last_boundary = offset;
        ++expected_sequence;
    }

    if (last_boundary != total) {
        // Drop the incomplete tail and trim the file back to the last record
        // boundary, so the next append starts from a clean edge instead of
        // after a fragment that would then sit in the middle of the log.
        discarded_tail_bytes_ = total - last_boundary;

        in.close();
        fs::resize_file(path_, last_boundary, error);
        if (error) {
            return Result::failure(
                StatusCode::IoError,
                "cannot trim the incomplete final log record: " + error.message());
        }
    }

    records = std::move(parsed);
    last_sequence_ = records.empty() ? 0 : records.back().sequence;
    tail_known_ = true;
    return Result::ok();
}

Result WriteAheadLog::reset() {
    tail_known_ = false;
    std::error_code error;
    const bool present = fs::exists(path_, error);
    if (error) {
        return Result::failure(StatusCode::IoError, "cannot inspect the log: " + error.message());
    }
    if (present) {
        // Truncating in place rather than deleting keeps the file, and any
        // permissions it carries, where it was.
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        if (!out) {
            return Result::failure(StatusCode::IoError, "cannot truncate the log");
        }
        out.flush();
        out.close();
        if (!out) {
            return Result::failure(StatusCode::IoError, "failed while truncating the log");
        }
    }

    last_sequence_ = 0;
    discarded_tail_bytes_ = 0;
    tail_known_ = true;
    return Result::ok();
}

}  // namespace minidb
