#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "minidb/database.hpp"
#include "minidb/storage.hpp"
#include "minidb/wal.hpp"
#include "temp_directory.hpp"
#include "test_framework.hpp"

using minidb::Database;
using minidb::StatusCode;
using minidb::WalMutation;
using minidb::WalOperation;
using minidb::WalRecord;
using minidb::WriteAheadLog;
using minidb::testing::read_file;
using minidb::testing::TempDirectory;
using minidb::testing::write_file;

namespace {
// Independent encoder: no production serialization helpers are used.
void integer(std::string& bytes, std::uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        bytes.push_back(static_cast<char>((value >> (8 * i)) & 255U));
    }
}
std::string record(std::uint16_t operation, std::uint64_t sequence, const std::string& key = {},
                   const std::string& value = {}) {
    std::string header = "MWAL";
    integer(header, 1, 2);
    integer(header, operation, 2);
    integer(header, sequence, 8);
    integer(header, key.size(), 4);
    integer(header, value.size(), 4);
    std::uint32_t crc = 0xffffffffU;
    for (char byte : header + key + value) {
        crc ^= static_cast<unsigned char>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
        }
    }
    integer(header, crc ^ 0xffffffffU, 4);
    return header + key + value;
}
void rejects(const std::string& bytes, StatusCode code = StatusCode::CorruptData) {
    const TempDirectory dir;
    const auto path = dir.file("log");
    ASSERT_TRUE(write_file(path, bytes));
    WriteAheadLog log(path);
    std::vector<WalRecord> records;
    EXPECT_EQ(log.replay(records).code(), code);
    EXPECT_TRUE(records.empty());
    EXPECT_EQ(read_file(path), bytes);
    EXPECT_FALSE(log.append_set("blocked", "value").is_ok());
}
}  // namespace

TEST(set_delete_clear_append_exact_binary_format) {
    const TempDirectory dir;
    WriteAheadLog log(dir.file("log"));
    ASSERT_TRUE(log.append_set("a", "value").is_ok());
    EXPECT_EQ(read_file(log.path()), record(1, 1, "a", "value"));
    ASSERT_TRUE(log.append_delete("a").is_ok());
    ASSERT_TRUE(log.append_clear().is_ok());
    EXPECT_EQ(read_file(log.path()), record(1, 1, "a", "value") + record(2, 2, "a") + record(3, 3));
}
TEST(replay_independently_encoded_binary_records) {
    const TempDirectory dir;
    const std::string key("a\0b", 3), value("\0\n\xff", 3);
    ASSERT_TRUE(write_file(dir.file("log"), record(1, 1, key, value) + record(2, 2, key)));
    WriteAheadLog log(dir.file("log"));
    std::vector<WalRecord> records;
    ASSERT_TRUE(log.replay(records).is_ok());
    ASSERT_TRUE(records.size() == 2);
    EXPECT_EQ(records[0].key, key);
    EXPECT_EQ(records[0].value, value);
    EXPECT_EQ(records[1].operation, WalOperation::Delete);
    EXPECT_EQ(log.last_sequence(), std::uint64_t{2});
    EXPECT_TRUE(log.append_set("next", "").is_ok());
    EXPECT_TRUE(log.replay(records).is_ok());
    EXPECT_EQ(records.size(), std::size_t{3});
}
TEST(transaction_record_replays_all_inner_mutations_with_one_sequence) {
    std::string payload;
    integer(payload, 2, 4);
    integer(payload, 1, 2);
    integer(payload, 1, 4);
    integer(payload, 1, 4);
    payload += "a1";
    integer(payload, 2, 2);
    integer(payload, 1, 4);
    integer(payload, 0, 4);
    payload += "b";

    const TempDirectory dir;
    ASSERT_TRUE(write_file(dir.file("log"), record(4, 1, {}, payload)));
    WriteAheadLog log(dir.file("log"));
    std::vector<WalRecord> records;
    ASSERT_TRUE(log.replay(records).is_ok());
    ASSERT_TRUE(records.size() == 2);
    EXPECT_EQ(records[0].operation, WalOperation::Set);
    EXPECT_EQ(records[0].key, std::string("a"));
    EXPECT_EQ(records[0].value, std::string("1"));
    EXPECT_EQ(records[0].sequence, std::uint64_t{1});
    EXPECT_EQ(records[1].operation, WalOperation::Delete);
    EXPECT_EQ(records[1].key, std::string("b"));
    EXPECT_EQ(records[1].sequence, std::uint64_t{1});
}
TEST(transaction_append_uses_one_outer_wal_record) {
    const TempDirectory dir;
    WriteAheadLog log(dir.file("log"));
    const std::vector<WalMutation> changes = {{WalOperation::Set, "a", "1"},
                                              {WalOperation::Delete, "b", {}}};
    ASSERT_TRUE(log.append_transaction(changes).is_ok());
    EXPECT_EQ(log.last_sequence(), std::uint64_t{1});
    EXPECT_EQ(read_file(log.path()).substr(6, 2), std::string("\x04\x00", 2));
}
TEST(malformed_transaction_payloads_are_rejected_after_valid_crc) {
    std::string zero_count;
    integer(zero_count, 0, 4);
    rejects(record(4, 1, {}, zero_count));

    std::string impossible_count;
    integer(impossible_count, 2, 4);
    integer(impossible_count, 1, 2);
    integer(impossible_count, 1, 4);
    integer(impossible_count, 0, 4);
    impossible_count += "a";
    rejects(record(4, 1, {}, impossible_count));

    std::string bad_operation;
    integer(bad_operation, 1, 4);
    integer(bad_operation, 99, 2);
    integer(bad_operation, 1, 4);
    integer(bad_operation, 0, 4);
    bad_operation += "a";
    rejects(record(4, 1, {}, bad_operation));

    std::string bad_lengths;
    integer(bad_lengths, 1, 4);
    integer(bad_lengths, 1, 2);
    integer(bad_lengths, 2, 4);
    integer(bad_lengths, 0, 4);
    bad_lengths += "a";
    rejects(record(4, 1, {}, bad_lengths));

    std::string trailing;
    integer(trailing, 1, 4);
    integer(trailing, 1, 2);
    integer(trailing, 1, 4);
    integer(trailing, 0, 4);
    trailing += "ax";
    rejects(record(4, 1, {}, trailing));
}
TEST(missing_and_empty_wal) {
    const TempDirectory dir;
    WriteAheadLog log(dir.file("log"));
    std::vector<WalRecord> records;
    EXPECT_EQ(log.replay(records).code(), StatusCode::NotFound);
    EXPECT_TRUE(records.empty());
    EXPECT_FALSE(std::filesystem::exists(log.path()));
    ASSERT_TRUE(write_file(log.path(), ""));
    EXPECT_TRUE(log.replay(records).is_ok());
    EXPECT_TRUE(records.empty());
    EXPECT_TRUE(log.append_set("a", "").is_ok());
}
TEST(every_incomplete_final_record_cut_is_trimmed_before_append) {
    const TempDirectory dir;
    const auto path = dir.file("log");
    const std::string first = record(1, 1, "first", "kept");
    const std::string final = record(1, 2, "last", "incomplete");
    for (std::size_t cut = 1; cut < final.size(); ++cut) {
        ASSERT_TRUE(write_file(path, first + final.substr(0, cut)));
        WriteAheadLog log(path);
        std::vector<WalRecord> records;
        ASSERT_TRUE(log.replay(records).is_ok());
        EXPECT_EQ(records.size(), std::size_t{1});
        EXPECT_EQ(log.discarded_tail_bytes(), static_cast<std::uint64_t>(cut));
        EXPECT_EQ(read_file(path), first);
        ASSERT_TRUE(log.append_delete("first").is_ok());
        EXPECT_TRUE(log.replay(records).is_ok());
        EXPECT_EQ(records.size(), std::size_t{2});
    }
}
TEST(incomplete_first_record_recovers_empty) {
    const TempDirectory dir;
    const std::string bytes = record(1, 1, "key", "value");
    for (std::size_t cut = 1; cut < bytes.size(); ++cut) {
        ASSERT_TRUE(write_file(dir.file("log"), bytes.substr(0, cut)));
        WriteAheadLog log(dir.file("log"));
        std::vector<WalRecord> records;
        EXPECT_TRUE(log.replay(records).is_ok());
        EXPECT_TRUE(records.empty());
        EXPECT_EQ(std::filesystem::file_size(log.path()), std::uintmax_t{0});
    }
}
TEST(corrupt_magic_version_operation_and_checksum) {
    const std::string valid = record(1, 1, "key", "value");
    for (const std::size_t offset :
         {std::size_t{0}, std::size_t{6}, std::size_t{24}, std::size_t{30}}) {
        std::string bytes = valid;
        bytes[offset] ^= 0x40;
        rejects(bytes);
    }
    std::string bytes = valid;
    bytes[4] = 2;
    rejects(bytes, StatusCode::UnsupportedVersion);
}
TEST(invalid_lengths_and_operation_payloads) {
    for (const std::size_t offset : {std::size_t{16}, std::size_t{20}}) {
        std::string bytes = record(1, 1, "a", "b");
        bytes.replace(offset, 4, 4, static_cast<char>(0xff));
        rejects(bytes);
    }
    rejects(record(1, 1, "", "value"));
    rejects(record(2, 1, "a", "forbidden"));
    rejects(record(3, 1, "forbidden"));
    rejects(record(1, 1, std::string(1025, 'k')));
}
TEST(sequence_gaps_duplicates_and_reordering_rejected) {
    rejects(record(1, 0, "a"));
    rejects(record(1, 2, "a"));
    rejects(record(1, 1, "a") + record(1, 1, "b"));
    rejects(record(1, 1, "a") + record(1, 3, "b"));
    rejects(record(1, UINT64_MAX, "a"));
}
TEST(corruption_after_valid_prefix_does_not_partly_recover) {
    std::string bad = record(1, 2, "b", "bad");
    bad.back() ^= 1;
    rejects(record(1, 1, "a", "good") + bad);
}
TEST(failed_replay_invalidates_previously_known_tail) {
    const TempDirectory dir;
    WriteAheadLog log(dir.file("log"));
    ASSERT_TRUE(log.append_set("a", "b").is_ok());
    ASSERT_TRUE(write_file(log.path(), std::string(28, 'x')));
    std::vector<WalRecord> records;
    EXPECT_FALSE(log.replay(records).is_ok());
    EXPECT_FALSE(log.append_set("c", "d").is_ok());
}
TEST(invalid_append_does_not_change_wal) {
    const TempDirectory dir;
    WriteAheadLog log(dir.file("log"));
    EXPECT_FALSE(log.append_set("", "b").is_ok());
    EXPECT_FALSE(log.append_delete("").is_ok());
    EXPECT_FALSE(log.append_set("a", std::string(minidb::limits::kMaxValueSize + 1, 'v')).is_ok());
    EXPECT_FALSE(log.exists());
}
TEST(size_limit_enforced_on_read_and_append) {
    const TempDirectory dir;
    WriteAheadLog log(dir.file("log"));
    ASSERT_TRUE(write_file(log.path(), ""));
    std::filesystem::resize_file(log.path(), minidb::limits::kMaxWalSize);
    EXPECT_FALSE(log.append_clear().is_ok());
    EXPECT_EQ(std::filesystem::file_size(log.path()), minidb::limits::kMaxWalSize);
    std::filesystem::resize_file(log.path(), minidb::limits::kMaxWalSize + 1);
    std::vector<WalRecord> records;
    EXPECT_EQ(log.replay(records).code(), StatusCode::CorruptData);
}
TEST(reset_restarts_sequence_and_missing_reset_is_safe) {
    const TempDirectory dir;
    WriteAheadLog log(dir.file("log"));
    EXPECT_TRUE(log.reset().is_ok());
    ASSERT_TRUE(log.append_set("a", "b").is_ok());
    ASSERT_TRUE(log.reset().is_ok());
    EXPECT_EQ(read_file(log.path()), std::string{});
    EXPECT_EQ(log.last_sequence(), std::uint64_t{0});
    ASSERT_TRUE(log.append_delete("a").is_ok());
    EXPECT_EQ(read_file(log.path()), record(2, 1, "a"));
}
TEST(directory_wal_reports_io_errors) {
    const TempDirectory dir;
    WriteAheadLog log(dir.path());
    std::vector<WalRecord> records;
    EXPECT_EQ(log.replay(records).code(), StatusCode::IoError);
    EXPECT_FALSE(log.append_clear().is_ok());
    EXPECT_EQ(log.reset().code(), StatusCode::IoError);
}
TEST(restart_without_snapshot_recovers_multiple_mutations) {
    const TempDirectory dir;
    const auto path = dir.file("db");
    {
        Database db(path);
        ASSERT_TRUE(db.set("a", "first").is_ok());
        ASSERT_TRUE(db.set("b", "delete").is_ok());
        ASSERT_TRUE(db.set("a", "second").is_ok());
        ASSERT_TRUE(db.remove("b").is_ok());
    }
    EXPECT_FALSE(std::filesystem::exists(path));
    Database recovered(path);
    ASSERT_TRUE(recovered.load().is_ok());
    EXPECT_EQ(recovered.size(), std::size_t{1});
    EXPECT_EQ(recovered.get("a").value(), std::string("second"));
    EXPECT_EQ(recovered.replayed_operation_count(), std::size_t{4});
}
TEST(snapshot_wal_save_reset_and_second_restart) {
    const TempDirectory dir;
    const auto path = dir.file("db");
    Database db(path);
    ASSERT_TRUE(db.set("base", "snapshot").is_ok());
    ASSERT_TRUE(db.save().is_ok());
    EXPECT_EQ(std::filesystem::file_size(db.wal_path()), std::uintmax_t{0});
    ASSERT_TRUE(db.set("base", "updated").is_ok());
    ASSERT_TRUE(db.set("new", "wal").is_ok());
    Database recovered(path);
    ASSERT_TRUE(recovered.load().is_ok());
    EXPECT_EQ(recovered.get("base").value(), std::string("updated"));
    EXPECT_EQ(recovered.get("new").value(), std::string("wal"));
    ASSERT_TRUE(recovered.save().is_ok());
    EXPECT_EQ(std::filesystem::file_size(recovered.wal_path()), std::uintmax_t{0});
    Database again(path);
    ASSERT_TRUE(again.load().is_ok());
    EXPECT_EQ(again.size(), std::size_t{2});
    EXPECT_EQ(again.replayed_operation_count(), std::size_t{0});
    ASSERT_TRUE(again.remove("base").is_ok());
    Database final(path);
    ASSERT_TRUE(final.load().is_ok());
    EXPECT_FALSE(final.exists("base"));
    EXPECT_TRUE(final.exists("new"));
}
TEST(snapshot_installed_before_reset_skips_checkpointed_records) {
    const TempDirectory dir;
    const auto path = dir.file("db");
    Database db(path);
    ASSERT_TRUE(db.set("old", "gone").is_ok());
    ASSERT_TRUE(db.save().is_ok());
    ASSERT_TRUE(db.clear().is_ok());
    ASSERT_TRUE(db.set("final", "value").is_ok());
    ASSERT_TRUE(db.set("drop", "value").is_ok());
    ASSERT_TRUE(db.remove("drop").is_ok());
    // Simulate the process stopping after snapshot replacement, before reset.
    ASSERT_TRUE(minidb::StorageManager(path).save({{"final", "value"}}, 5).is_ok());
    Database recovered(path);
    ASSERT_TRUE(recovered.load().is_ok());
    EXPECT_EQ(recovered.size(), std::size_t{1});
    EXPECT_EQ(recovered.get("final").value(), std::string("value"));
    EXPECT_EQ(recovered.replayed_operation_count(), std::size_t{0});
    ASSERT_TRUE(recovered.save().is_ok());
    EXPECT_EQ(read_file(recovered.wal_path()), std::string{});
}
TEST(failed_snapshot_preserves_wal) {
    const TempDirectory dir;
    Database db(dir.file("db"));
    ASSERT_TRUE(db.set("a", "recoverable").is_ok());
    const std::string before = read_file(db.wal_path());
    std::filesystem::create_directory(dir.file("db.tmp"));
    EXPECT_EQ(db.save().code(), StatusCode::IoError);
    EXPECT_EQ(read_file(db.wal_path()), before);
    Database recovered(dir.file("db"));
    ASSERT_TRUE(recovered.load().is_ok());
    EXPECT_EQ(recovered.get("a").value(), std::string("recoverable"));
}
TEST(failed_append_does_not_change_memory) {
    const TempDirectory dir;
    Database db(dir.file("db"));
    ASSERT_TRUE(db.set("a", "old").is_ok());
    ASSERT_TRUE(db.save().is_ok());
    std::filesystem::remove(db.wal_path());
    std::filesystem::create_directory(db.wal_path());
    EXPECT_FALSE(db.set("a", "new").is_ok());
    EXPECT_EQ(db.get("a").value(), std::string("old"));
    EXPECT_FALSE(db.remove("a").is_ok());
    EXPECT_FALSE(db.clear().is_ok());
    EXPECT_EQ(db.size(), std::size_t{1});
    EXPECT_FALSE(db.save().is_ok());
}
TEST(corrupt_wal_preserves_memory_and_blocks_save) {
    const TempDirectory dir;
    Database db(dir.file("db"));
    ASSERT_TRUE(db.set("a", "old").is_ok());
    ASSERT_TRUE(write_file(db.wal_path(), std::string(28, 'x')));
    EXPECT_EQ(db.load().code(), StatusCode::CorruptData);
    EXPECT_EQ(db.get("a").value(), std::string("old"));
    EXPECT_FALSE(db.save().is_ok());
    EXPECT_FALSE(db.set("a", "new").is_ok());
    EXPECT_EQ(read_file(db.wal_path()), std::string(28, 'x'));
}
TEST(first_save_or_mutation_recovers_existing_files) {
    const TempDirectory dir;
    const auto path = dir.file("db");
    {
        Database db(path);
        ASSERT_TRUE(db.set("kept", "wal").is_ok());
    }
    {
        Database db(path);
        ASSERT_TRUE(db.save().is_ok());
    }
    {
        Database db(path);
        ASSERT_TRUE(db.set("new", "value").is_ok());
    }
    Database db(path);
    ASSERT_TRUE(db.load().is_ok());
    EXPECT_EQ(db.get("kept").value(), std::string("wal"));
    EXPECT_EQ(db.get("new").value(), std::string("value"));
}
TEST(database_recovers_tail_and_continues_without_relogging) {
    const TempDirectory dir;
    Database db(dir.file("db"));
    const std::string prefix = record(1, 1, "kept", "value");
    ASSERT_TRUE(write_file(db.wal_path(), prefix + record(2, 2, "kept").substr(0, 29)));
    ASSERT_TRUE(db.load().is_ok());
    EXPECT_EQ(read_file(db.wal_path()), prefix);
    EXPECT_TRUE(db.exists("kept"));
    ASSERT_TRUE(db.remove("kept").is_ok());
    Database recovered(dir.file("db"));
    ASSERT_TRUE(recovered.load().is_ok());
    EXPECT_TRUE(recovered.empty());
}
TEST(failed_reset_reports_failure_after_snapshot_installation) {
    const TempDirectory dir;
    Database db(dir.file("db"));
    ASSERT_TRUE(db.set("a", "value").is_ok());
    const std::string old_log = read_file(db.wal_path());
    std::filesystem::remove(db.wal_path());
    std::filesystem::create_directory(db.wal_path());
    EXPECT_EQ(db.save().code(), StatusCode::IoError);
    std::vector<minidb::Record> snapshot;
    ASSERT_TRUE(minidb::StorageManager(db.snapshot_path()).load(snapshot).is_ok());
    ASSERT_TRUE(snapshot.size() == 1);
    EXPECT_EQ(snapshot[0].key, std::string("a"));
    EXPECT_FALSE(db.set("b", "blocked").is_ok());
    std::filesystem::remove(db.wal_path());
    ASSERT_TRUE(write_file(db.wal_path(), old_log));
    ASSERT_TRUE(db.load().is_ok());
    ASSERT_TRUE(db.save().is_ok());
    EXPECT_EQ(read_file(db.wal_path()), std::string{});
}
TEST(unknown_wal_tail_requires_replay) {
    const TempDirectory dir;
    ASSERT_TRUE(write_file(dir.file("log"), record(1, 1, "a", "b")));
    WriteAheadLog log(dir.file("log"));
    EXPECT_FALSE(log.append_delete("a").is_ok());
    std::vector<WalRecord> records;
    ASSERT_TRUE(log.replay(records).is_ok());
    EXPECT_TRUE(log.append_delete("a").is_ok());
}
TEST(missing_key_delete_does_not_append) {
    const TempDirectory dir;
    Database db(dir.file("db"));
    ASSERT_TRUE(db.set("a", "b").is_ok());
    const std::string before = read_file(db.wal_path());
    EXPECT_EQ(db.remove("absent").code(), StatusCode::NotFound);
    EXPECT_EQ(read_file(db.wal_path()), before);
}
TEST(oversized_snapshot_is_refused_before_wal_reset) {
    const TempDirectory dir;
    const auto path = dir.file("db");
    ASSERT_TRUE(minidb::StorageManager(path).save({{"old", "snapshot"}}).is_ok());
    const std::string before = read_file(path);
    const std::string value(minidb::limits::kMaxValueSize, 'v');
    std::vector<minidb::Record> records;
    for (int i = 0; i < 256; ++i) {
        records.push_back({std::to_string(i), value});
    }
    EXPECT_EQ(minidb::StorageManager(path).save(records).code(), StatusCode::InvalidArgument);
    EXPECT_EQ(read_file(path), before);
    EXPECT_FALSE(std::filesystem::exists(dir.file("db.tmp")));
}
MINIDB_TEST_MAIN()
