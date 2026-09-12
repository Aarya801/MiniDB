// Tests for the snapshot format and StorageManager.
//
// The corruption tests build snapshot bytes by hand rather than by calling
// save(). That is deliberate: the builder below is an independent
// re-implementation of the layout in docs/STORAGE_FORMAT.md, so if the
// production writer and the documented format ever disagree, these tests
// notice. Crafting the bytes is also the only way to produce files save()
// would never write -- a bad magic number, a duplicated key, a count that
// does not match the contents.

#include "minidb/storage.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "minidb/result.hpp"
#include "minidb/types.hpp"
#include "temp_directory.hpp"
#include "test_framework.hpp"

using minidb::Record;
using minidb::Result;
using minidb::StatusCode;
using minidb::StorageManager;
using minidb::testing::read_file;
using minidb::testing::TempDirectory;
using minidb::testing::write_file;

namespace {

// -------------------------------------------------------------------------
// A hand-built snapshot, following docs/STORAGE_FORMAT.md
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

/// CRC-32 as the format specifies it. Deliberately a second implementation,
/// independent of the one in src/storage.cpp: if the two ever disagree, the
/// round-trip tests below stop passing.
[[nodiscard]] std::uint32_t crc32(const std::string& data) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const char byte : data) {
        crc ^= static_cast<std::uint32_t>(static_cast<unsigned char>(byte));
        for (int bit = 0; bit < 8; ++bit) {
            crc = ((crc & 1U) != 0U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

/// Builds snapshot bytes with every header field settable, so a test can
/// break exactly one thing and leave the rest well formed.
///
/// The checksum is computed over whatever payload the builder produces unless
/// a test overrides it. That matters: with a fixed placeholder instead, every
/// hand-built file would fail the checksum check and each corruption test
/// would pass without ever reaching the rule it claims to exercise.
struct SnapshotBuilder {
    std::string magic{StorageManager::kMagic, sizeof(StorageManager::kMagic)};
    std::uint32_t version = StorageManager::kFormatVersion;

    /// Overrides the checksum written into the header. Left unset, the real
    /// checksum of the payload is used.
    bool override_checksum = false;
    std::uint32_t checksum_override = 0;

    /// Overrides the record count written into the header. Left unset, the
    /// real number of records is used.
    bool override_count = false;
    std::uint64_t count_override = 0;

    /// Records as raw lengths plus bytes, so a length can disagree with the
    /// data that follows it.
    struct RawRecord {
        std::uint32_t key_length;
        std::uint32_t value_length;
        std::string key;
        std::string value;
    };
    std::vector<RawRecord> records;

    void add(const std::string& key, const std::string& value) {
        records.push_back(RawRecord{static_cast<std::uint32_t>(key.size()),
                                    static_cast<std::uint32_t>(value.size()), key, value});
    }

    [[nodiscard]] std::string payload() const {
        std::string out;
        for (const RawRecord& record : records) {
            append_u32(out, record.key_length);
            append_u32(out, record.value_length);
            out.append(record.key);
            out.append(record.value);
        }
        return out;
    }

    [[nodiscard]] std::string bytes() const {
        const std::string body = payload();

        std::string out = magic;
        append_u32(out, version);
        append_u64(out,
                   override_count ? count_override : static_cast<std::uint64_t>(records.size()));
        append_u32(out, override_checksum ? checksum_override : crc32(body));
        out.append(body);
        return out;
    }
};

/// Records as "key=value" pairs, sorted and joined, for readable comparisons.
std::string describe(std::vector<Record> records) {
    std::sort(records.begin(), records.end(),
              [](const Record& left, const Record& right) { return left.key < right.key; });

    std::string joined;
    for (const Record& record : records) {
        if (!joined.empty()) {
            joined += ',';
        }
        joined += record.key;
        joined += '=';
        joined += record.value;
    }
    return joined;
}

/// Saves `records`, reads them back, and returns the round-tripped set.
/// Reports through the framework if either half fails.
std::string round_trip(const StorageManager& storage, const std::vector<Record>& records) {
    const Result saved = storage.save(records);
    EXPECT_TRUE(saved.is_ok());

    std::vector<Record> loaded;
    const Result read = storage.load(loaded);
    EXPECT_TRUE(read.is_ok());
    return describe(std::move(loaded));
}

/// Writes hand-built bytes to a snapshot and returns what load() made of it.
Result load_outcome(const SnapshotBuilder& builder, const std::filesystem::path& path) {
    EXPECT_TRUE(write_file(path, builder.bytes()));

    const StorageManager storage(path);
    std::vector<Record> loaded;
    Result read = storage.load(loaded);
    // A rejected snapshot must never leave records behind for the caller.
    EXPECT_TRUE(loaded.empty());
    return read;
}

/// Checks that a hand-built snapshot is rejected, and that it was rejected for
/// the stated reason rather than by some later check that happens to share the
/// same status code.
void expect_rejected(const SnapshotBuilder& builder, const std::filesystem::path& path,
                     StatusCode expected_code, std::string_view expected_reason) {
    const Result read = load_outcome(builder, path);
    EXPECT_EQ(read.code(), expected_code);
    EXPECT_TRUE(read.message().find(expected_reason) != std::string::npos);
}

}  // namespace

// -------------------------------------------------------------------------
// Round trips
// -------------------------------------------------------------------------

TEST(an_empty_database_saves_and_reloads_as_empty) {
    const TempDirectory directory;
    const StorageManager storage(directory.file("db"));

    EXPECT_EQ(round_trip(storage, {}), std::string(""));
    // Even with no records the file exists and carries a valid header.
    EXPECT_TRUE(storage.snapshot_exists());
    EXPECT_EQ(read_file(storage.path()).size(), StorageManager::kHeaderSize);
}

TEST(a_single_record_survives_a_round_trip) {
    const TempDirectory directory;
    const StorageManager storage(directory.file("db"));

    EXPECT_EQ(round_trip(storage, {{"name", "Aarya"}}), std::string("name=Aarya"));
}

TEST(several_records_survive_a_round_trip) {
    const TempDirectory directory;
    const StorageManager storage(directory.file("db"));

    EXPECT_EQ(round_trip(storage, {{"name", "Aarya"}, {"language", "C++"}, {"age", "18"}}),
              std::string("age=18,language=C++,name=Aarya"));
}

TEST(values_containing_spaces_survive_a_round_trip) {
    const TempDirectory directory;
    const StorageManager storage(directory.file("db"));

    EXPECT_EQ(round_trip(storage, {{"note", "hello world with spaces"}, {"pad", "  padded  "}}),
              std::string("note=hello world with spaces,pad=  padded  "));
}

TEST(an_empty_value_survives_a_round_trip) {
    const TempDirectory directory;
    const StorageManager storage(directory.file("db"));

    EXPECT_EQ(round_trip(storage, {{"blank", ""}}), std::string("blank="));
}

TEST(binary_values_survive_a_round_trip) {
    // The format stores lengths, not terminators, so an embedded NUL is data
    // like any other byte.
    const TempDirectory directory;
    const StorageManager storage(directory.file("db"));
    const std::string binary("a\0b\0c", 5);

    EXPECT_TRUE(storage.save({{"bin", binary}}).is_ok());

    std::vector<Record> loaded;
    ASSERT_TRUE(storage.load(loaded).is_ok());
    ASSERT_TRUE(loaded.size() == 1);
    EXPECT_EQ(loaded[0].value.size(), static_cast<std::size_t>(5));
    EXPECT_EQ(loaded[0].value, binary);
}

TEST(a_long_value_survives_a_round_trip) {
    const TempDirectory directory;
    const StorageManager storage(directory.file("db"));
    const std::string long_value(100000, 'x');

    EXPECT_TRUE(storage.save({{"big", long_value}}).is_ok());

    std::vector<Record> loaded;
    ASSERT_TRUE(storage.load(loaded).is_ok());
    ASSERT_TRUE(loaded.size() == 1);
    EXPECT_EQ(loaded[0].value.size(), long_value.size());
    EXPECT_EQ(loaded[0].value, long_value);
}

TEST(keys_at_the_size_limit_survive_a_round_trip) {
    const TempDirectory directory;
    const StorageManager storage(directory.file("db"));
    const std::string boundary_key(minidb::limits::kMaxKeySize, 'k');

    EXPECT_TRUE(storage.save({{boundary_key, "v"}}).is_ok());

    std::vector<Record> loaded;
    ASSERT_TRUE(storage.load(loaded).is_ok());
    ASSERT_TRUE(loaded.size() == 1);
    EXPECT_EQ(loaded[0].key, boundary_key);
}

TEST(many_records_survive_a_round_trip) {
    const TempDirectory directory;
    const StorageManager storage(directory.file("db"));

    std::vector<Record> records;
    for (int index = 0; index < 5000; ++index) {
        records.push_back(Record{"key" + std::to_string(index), "value" + std::to_string(index)});
    }
    EXPECT_TRUE(storage.save(records).is_ok());

    std::vector<Record> loaded;
    ASSERT_TRUE(storage.load(loaded).is_ok());
    EXPECT_EQ(loaded.size(), records.size());
    EXPECT_EQ(describe(std::move(loaded)), describe(records));
}

TEST(saving_again_replaces_the_previous_snapshot) {
    const TempDirectory directory;
    const StorageManager storage(directory.file("db"));

    EXPECT_TRUE(storage.save({{"a", "1"}, {"b", "2"}}).is_ok());
    // Fewer records than before: the file must not keep the old ones.
    EXPECT_EQ(round_trip(storage, {{"a", "updated"}}), std::string("a=updated"));
}

// -------------------------------------------------------------------------
// Missing snapshot
// -------------------------------------------------------------------------

TEST(a_missing_snapshot_reports_not_found_rather_than_an_error) {
    const TempDirectory directory;
    const StorageManager storage(directory.file("never-written"));

    EXPECT_FALSE(storage.snapshot_exists());

    std::vector<Record> loaded;
    const Result read = storage.load(loaded);
    // NotFound is the signal callers turn into "start with an empty
    // database"; it must be distinguishable from corruption.
    EXPECT_EQ(read.code(), StatusCode::NotFound);
    EXPECT_TRUE(loaded.empty());
}

TEST(saving_creates_missing_parent_directories) {
    const TempDirectory directory;
    const StorageManager storage(directory.file("nested") / "deeper" / "db");

    EXPECT_TRUE(storage.save({{"k", "v"}}).is_ok());
    EXPECT_TRUE(storage.snapshot_exists());
}

// -------------------------------------------------------------------------
// Corruption: the header
// -------------------------------------------------------------------------

TEST(a_file_that_is_not_a_snapshot_is_rejected) {
    const TempDirectory directory;
    const std::filesystem::path path = directory.file("db");
    EXPECT_TRUE(write_file(path, "this is a text file, not a MiniDB snapshot at all"));

    const StorageManager storage(path);
    std::vector<Record> loaded;
    const Result read = storage.load(loaded);
    EXPECT_EQ(read.code(), StatusCode::CorruptData);
    EXPECT_TRUE(loaded.empty());
}

TEST(a_wrong_magic_number_is_rejected) {
    const TempDirectory directory;
    SnapshotBuilder builder;
    builder.magic = "NOTMINID";
    builder.add("k", "v");

    expect_rejected(builder, directory.file("db"), StatusCode::CorruptData, "magic number");
}

TEST(an_unsupported_version_is_rejected_as_such) {
    const TempDirectory directory;
    SnapshotBuilder builder;
    builder.version = StorageManager::kCheckpointVersion + 1;
    builder.add("k", "v");

    // Distinct from CorruptData: the file may be perfectly well formed, just
    // written by a newer MiniDB. Telling those apart is the point of having
    // a version field.
    expect_rejected(builder, directory.file("db"), StatusCode::UnsupportedVersion,
                    "is not supported");
}

TEST(an_empty_file_is_rejected) {
    const TempDirectory directory;
    const std::filesystem::path path = directory.file("db");
    EXPECT_TRUE(write_file(path, ""));

    const StorageManager storage(path);
    std::vector<Record> loaded;
    // A zero-length file is not an empty database -- an empty database still
    // has a header.
    EXPECT_EQ(storage.load(loaded).code(), StatusCode::CorruptData);
}

TEST(a_truncated_header_is_rejected) {
    const TempDirectory directory;
    const std::filesystem::path path = directory.file("db");

    SnapshotBuilder builder;
    builder.add("k", "v");
    const std::string full = builder.bytes();

    for (std::size_t length = 1; length < StorageManager::kHeaderSize; ++length) {
        EXPECT_TRUE(write_file(path, full.substr(0, length)));
        const StorageManager storage(path);
        std::vector<Record> loaded;
        EXPECT_EQ(storage.load(loaded).code(), StatusCode::CorruptData);
        EXPECT_TRUE(loaded.empty());
    }
}

// -------------------------------------------------------------------------
// Corruption: the record count
// -------------------------------------------------------------------------

TEST(a_record_count_larger_than_the_file_is_rejected) {
    const TempDirectory directory;
    SnapshotBuilder builder;
    builder.add("k", "v");
    builder.override_count = true;
    builder.count_override = 1000;

    expect_rejected(builder, directory.file("db"), StatusCode::CorruptData,
                    "than its size can hold");
}

TEST(an_absurd_record_count_is_rejected_without_allocating) {
    // The check divides the available bytes rather than multiplying the
    // count, so a count near the limit of its type cannot overflow the
    // arithmetic or reach a reserve() call.
    const TempDirectory directory;
    SnapshotBuilder builder;
    builder.add("k", "v");
    builder.override_count = true;
    builder.count_override = 0xFFFFFFFFFFFFFFFFULL;

    expect_rejected(builder, directory.file("db"), StatusCode::CorruptData,
                    "than the limit allows");
}

TEST(a_count_beyond_the_configured_limit_is_rejected) {
    const TempDirectory directory;
    SnapshotBuilder builder;
    builder.add("k", "v");
    builder.override_count = true;
    builder.count_override = minidb::limits::kMaxRecordCount + 1;

    expect_rejected(builder, directory.file("db"), StatusCode::CorruptData,
                    "than the limit allows");
}

TEST(a_record_count_lower_than_the_contents_is_rejected) {
    // Trailing bytes mean the file is not what its header describes, even
    // though every record parsed cleanly.
    const TempDirectory directory;
    SnapshotBuilder builder;
    builder.add("a", "1");
    builder.add("b", "2");
    builder.override_count = true;
    builder.count_override = 1;

    expect_rejected(builder, directory.file("db"), StatusCode::CorruptData, "trailing bytes");
}

// -------------------------------------------------------------------------
// Corruption: record lengths
// -------------------------------------------------------------------------

TEST(a_zero_key_length_is_rejected) {
    const TempDirectory directory;
    SnapshotBuilder builder;
    builder.records.push_back({0, 1, "", "v"});

    expect_rejected(builder, directory.file("db"), StatusCode::CorruptData, "invalid key length");
}

TEST(a_key_length_beyond_the_limit_is_rejected) {
    const TempDirectory directory;
    SnapshotBuilder builder;
    builder.records.push_back(
        {static_cast<std::uint32_t>(minidb::limits::kMaxKeySize + 1), 0, "k", ""});

    expect_rejected(builder, directory.file("db"), StatusCode::CorruptData, "invalid key length");
}

TEST(a_value_length_beyond_the_limit_is_rejected) {
    const TempDirectory directory;
    SnapshotBuilder builder;
    builder.records.push_back(
        {1, static_cast<std::uint32_t>(minidb::limits::kMaxValueSize + 1), "k", ""});

    expect_rejected(builder, directory.file("db"), StatusCode::CorruptData, "invalid value length");
}

TEST(lengths_that_exceed_the_remaining_file_are_rejected) {
    // The classic malicious record: a plausible length with nothing behind
    // it. Reading it must not resize a string to a number the file cannot
    // back up.
    const TempDirectory directory;
    SnapshotBuilder builder;
    builder.records.push_back({8, 500000, "realkey", ""});

    expect_rejected(builder, directory.file("db"), StatusCode::CorruptData,
                    "more bytes than the snapshot contains");
}

TEST(maximum_length_fields_cannot_overflow_the_size_arithmetic) {
    // Both length fields at the top of their range. Summed as 64-bit values
    // this cannot wrap, so the comparison against the remaining bytes still
    // holds and the file is refused.
    const TempDirectory directory;
    SnapshotBuilder builder;
    builder.records.push_back({0xFFFFFFFFU, 0xFFFFFFFFU, "k", "v"});

    expect_rejected(builder, directory.file("db"), StatusCode::CorruptData, "invalid key length");
}

TEST(a_truncated_record_is_rejected_at_every_cut_point) {
    const TempDirectory directory;
    const std::filesystem::path path = directory.file("db");

    const StorageManager writer(path);
    EXPECT_TRUE(writer.save({{"alpha", "first"}, {"beta", "second"}}).is_ok());
    const std::string full = read_file(path);
    ASSERT_TRUE(full.size() > StorageManager::kHeaderSize);

    // Every truncation past the header must be caught, whether it lands in a
    // length field, a key, or a value.
    for (std::size_t length = StorageManager::kHeaderSize; length < full.size(); ++length) {
        EXPECT_TRUE(write_file(path, full.substr(0, length)));
        const StorageManager storage(path);
        std::vector<Record> loaded;
        EXPECT_EQ(storage.load(loaded).code(), StatusCode::CorruptData);
        EXPECT_TRUE(loaded.empty());
    }
}

// -------------------------------------------------------------------------
// Corruption: duplicates and checksum
// -------------------------------------------------------------------------

TEST(duplicate_keys_in_a_snapshot_are_rejected) {
    // save() cannot produce this, because the database holds each key once.
    // A file containing it has been altered, and silently keeping whichever
    // record came last would be exactly the quiet data loss to avoid.
    const TempDirectory directory;
    SnapshotBuilder builder;
    builder.add("same", "first");
    builder.add("same", "second");

    expect_rejected(builder, directory.file("db"), StatusCode::CorruptData, "twice");
}

TEST(a_flipped_bit_inside_a_value_is_caught_by_the_checksum) {
    // Nothing structural is wrong here: the magic, version, count and every
    // length are intact, and the record parses. Only the checksum notices.
    const TempDirectory directory;
    const std::filesystem::path path = directory.file("db");

    const StorageManager storage(path);
    EXPECT_TRUE(storage.save({{"key", "value"}}).is_ok());

    std::string bytes = read_file(path);
    ASSERT_TRUE(!bytes.empty());
    bytes[bytes.size() - 1] = static_cast<char>(bytes[bytes.size() - 1] ^ 0x01);
    EXPECT_TRUE(write_file(path, bytes));

    std::vector<Record> loaded;
    EXPECT_EQ(storage.load(loaded).code(), StatusCode::CorruptData);
    EXPECT_TRUE(loaded.empty());
}

TEST(a_flipped_bit_inside_a_key_is_caught_by_the_checksum) {
    const TempDirectory directory;
    const std::filesystem::path path = directory.file("db");

    const StorageManager storage(path);
    EXPECT_TRUE(storage.save({{"abcdefgh", "value"}}).is_ok());

    std::string bytes = read_file(path);
    // The first key byte sits right after the header and the two lengths.
    const std::size_t key_offset = StorageManager::kHeaderSize + 8;
    ASSERT_TRUE(bytes.size() > key_offset);
    bytes[key_offset] = static_cast<char>(bytes[key_offset] ^ 0x20);
    EXPECT_TRUE(write_file(path, bytes));

    std::vector<Record> loaded;
    EXPECT_EQ(storage.load(loaded).code(), StatusCode::CorruptData);
}

TEST(a_wrong_checksum_is_rejected_even_when_the_records_parse) {
    const TempDirectory directory;
    SnapshotBuilder builder;
    builder.add("k", "v");
    builder.override_checksum = true;
    builder.checksum_override = 0xDEADBEEFU;

    expect_rejected(builder, directory.file("db"), StatusCode::CorruptData,
                    "checksum does not match");
}

// -------------------------------------------------------------------------
// Write failures and the scratch file
// -------------------------------------------------------------------------

TEST(a_successful_save_leaves_no_scratch_file) {
    const TempDirectory directory;
    const StorageManager storage(directory.file("db"));

    EXPECT_TRUE(storage.save({{"k", "v"}}).is_ok());

    EXPECT_TRUE(storage.snapshot_exists());
    EXPECT_FALSE(std::filesystem::exists(storage.temporary_path()));
}

TEST(a_failed_save_leaves_no_scratch_file_and_no_snapshot) {
    // The parent of the snapshot is a regular file, so the directory cannot
    // be created and the save must fail cleanly.
    const TempDirectory directory;
    const std::filesystem::path blocker = directory.file("blocker");
    EXPECT_TRUE(write_file(blocker, "not a directory"));

    const StorageManager storage(blocker / "db");
    const Result saved = storage.save({{"k", "v"}});

    EXPECT_FALSE(saved.is_ok());
    EXPECT_EQ(saved.code(), StatusCode::IoError);
    EXPECT_FALSE(std::filesystem::exists(storage.temporary_path()));
}

TEST(a_failed_save_leaves_the_previous_snapshot_intact) {
    // Replacement happens in one step, so a save that cannot complete must
    // not damage what was already stored.
    const TempDirectory directory;
    const StorageManager storage(directory.file("db"));
    EXPECT_TRUE(storage.save({{"original", "data"}}).is_ok());

    // A key the writer refuses, so save() fails before touching the file.
    const std::vector<Record> rejected = {{std::string(minidb::limits::kMaxKeySize + 1, 'k'), "v"}};
    EXPECT_FALSE(storage.save(rejected).is_ok());

    std::vector<Record> loaded;
    ASSERT_TRUE(storage.load(loaded).is_ok());
    EXPECT_EQ(describe(std::move(loaded)), std::string("original=data"));
}

TEST(saving_refuses_records_that_could_not_be_read_back) {
    const TempDirectory directory;
    const StorageManager storage(directory.file("db"));

    EXPECT_EQ(storage.save({{"", "v"}}).code(), StatusCode::KeyTooLarge);
    EXPECT_EQ(storage.save({{std::string(minidb::limits::kMaxKeySize + 1, 'k'), "v"}}).code(),
              StatusCode::KeyTooLarge);
    EXPECT_EQ(storage.save({{"k", std::string(minidb::limits::kMaxValueSize + 1, 'v')}}).code(),
              StatusCode::ValueTooLarge);

    // None of the refusals created a file.
    EXPECT_FALSE(storage.snapshot_exists());
}

TEST(a_directory_in_place_of_a_snapshot_is_reported_not_silently_ignored) {
    const TempDirectory directory;
    const std::filesystem::path path = directory.file("db");
    std::error_code ignored;
    std::filesystem::create_directories(path, ignored);

    const StorageManager storage(path);
    EXPECT_FALSE(storage.snapshot_exists());

    std::vector<Record> loaded;
    // Must not be reported as NotFound: something is there, it is just not a
    // snapshot, and treating it as "new database" would then try to replace
    // a directory on save.
    EXPECT_EQ(storage.load(loaded).code(), StatusCode::IoError);
}

// -------------------------------------------------------------------------
// The default database location
// -------------------------------------------------------------------------

TEST(the_default_snapshot_path_is_named_and_outside_the_repository) {
    const std::filesystem::path path = minidb::default_snapshot_path();

    EXPECT_FALSE(path.empty());
    EXPECT_EQ(path.filename().string(), std::string("minidb.snapshot"));
    // No assertion about the directory: it comes from the environment, which
    // differs per machine and per platform. Asserting it would be asserting
    // the test runner's configuration.
}

MINIDB_TEST_MAIN()
