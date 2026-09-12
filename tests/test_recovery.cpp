#include "minidb/database.hpp"
#include "minidb/storage.hpp"
#include "minidb/wal.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include "temp_directory.hpp"
#include "test_framework.hpp"

using minidb::Database;
using minidb::Record;
using minidb::StatusCode;
using minidb::StorageManager;
using minidb::WalRecord;
using minidb::WriteAheadLog;
using minidb::testing::read_file;
using minidb::testing::TempDirectory;
using minidb::testing::write_file;

namespace {
void expect_state(const Database& db) {
    EXPECT_EQ(db.size(), std::size_t{2});
    EXPECT_FALSE(db.exists("a"));
    EXPECT_EQ(db.get("b").value(), std::string("2"));
    EXPECT_EQ(db.get("c").value(), std::string("3"));
}
void mutations(Database& db) {
    ASSERT_TRUE(db.set("a", "1").is_ok());
    ASSERT_TRUE(db.set("b", "2").is_ok());
    ASSERT_TRUE(db.remove("a").is_ok());
    ASSERT_TRUE(db.set("c", "3").is_ok());
}
}  // namespace

TEST(recovery_exact_requested_sequence_without_snapshot) {
    const TempDirectory dir;
    {
        Database writer(dir.file("db"));
        mutations(writer);
    }
    Database reader(dir.file("db"));
    ASSERT_TRUE(reader.open().is_ok());
    expect_state(reader);
    EXPECT_EQ(reader.replayed_operation_count(), std::size_t{4});
    const std::string before = read_file(reader.wal_path());
    ASSERT_TRUE(reader.open().is_ok());
    expect_state(reader);
    EXPECT_EQ(read_file(reader.wal_path()), before);
}
TEST(recovery_snapshot_then_only_new_operations) {
    const TempDirectory dir;
    Database db(dir.file("db"));
    ASSERT_TRUE(db.set("a", "1").is_ok());
    ASSERT_TRUE(db.set("b", "2").is_ok());
    ASSERT_TRUE(db.save().is_ok());
    std::uint64_t checkpoint = 0;
    std::vector<Record> snapshot;
    ASSERT_TRUE(StorageManager(db.snapshot_path()).load(snapshot, &checkpoint).is_ok());
    EXPECT_EQ(checkpoint, std::uint64_t{2});
    EXPECT_EQ(snapshot.size(), std::size_t{2});
    EXPECT_EQ(read_file(db.wal_path()), std::string{});
    ASSERT_TRUE(db.remove("a").is_ok());
    ASSERT_TRUE(db.set("c", "3").is_ok());
    WriteAheadLog log(db.wal_path());
    std::vector<WalRecord> records;
    ASSERT_TRUE(log.replay(records, checkpoint).is_ok());
    ASSERT_TRUE(records.size() == 2);
    EXPECT_EQ(records.front().sequence, std::uint64_t{3});
    EXPECT_EQ(records.back().sequence, std::uint64_t{4});
    Database reader(db.snapshot_path());
    ASSERT_TRUE(reader.open().is_ok());
    expect_state(reader);
    EXPECT_EQ(reader.replayed_operation_count(), std::size_t{2});
}
TEST(recovery_crash_after_snapshot_install_skips_entire_wal) {
    const TempDirectory dir;
    Database db(dir.file("db"));
    mutations(db);
    // Simulate stopping after the atomic snapshot replacement, before reset.
    ASSERT_TRUE(StorageManager(db.snapshot_path()).save({{"b", "2"}, {"c", "3"}}, 4).is_ok());
    Database reader(db.snapshot_path());
    ASSERT_TRUE(reader.open().is_ok());
    expect_state(reader);
    EXPECT_EQ(reader.replayed_operation_count(), std::size_t{0});
    ASSERT_TRUE(reader.set("later", "5").is_ok());
    Database again(db.snapshot_path());
    ASSERT_TRUE(again.open().is_ok());
    EXPECT_EQ(again.replayed_operation_count(), std::size_t{1});
    EXPECT_EQ(again.get("later").value(), std::string("5"));
    ASSERT_TRUE(again.save().is_ok());
    EXPECT_EQ(read_file(again.wal_path()), std::string{});
}
TEST(recovery_skips_clear_and_set_already_in_snapshot) {
    const TempDirectory dir;
    WriteAheadLog log(dir.file("db.wal"));
    ASSERT_TRUE(log.append_clear().is_ok());
    ASSERT_TRUE(log.append_set("a", "old").is_ok());
    // Deliberately different state makes unintended replay observable.
    ASSERT_TRUE(
        StorageManager(dir.file("db")).save({{"a", "snapshot"}, {"keep", "yes"}}, 2).is_ok());
    ASSERT_TRUE(log.append_set("new", "after").is_ok());
    Database db(dir.file("db"));
    ASSERT_TRUE(db.open().is_ok());
    EXPECT_EQ(db.get("a").value(), std::string("snapshot"));
    EXPECT_TRUE(db.exists("keep"));
    EXPECT_EQ(db.get("new").value(), std::string("after"));
    EXPECT_EQ(db.replayed_operation_count(), std::size_t{1});
}
TEST(recovery_missing_or_empty_wal_preserves_checkpoint) {
    const TempDirectory dir;
    for (bool empty : {false, true}) {
        const auto path = dir.file(empty ? "empty" : "missing");
        ASSERT_TRUE(StorageManager(path).save({{"saved", "yes"}}, 17).is_ok());
        Database db(path);
        if (empty) {
            ASSERT_TRUE(write_file(db.wal_path(), ""));
        }
        ASSERT_TRUE(db.open().is_ok());
        EXPECT_EQ(db.replayed_operation_count(), std::size_t{0});
        EXPECT_EQ(db.get("saved").value(), std::string("yes"));
        ASSERT_TRUE(db.set("next", "18").is_ok());
        Database again(path);
        ASSERT_TRUE(again.open().is_ok());
        EXPECT_EQ(again.get("next").value(), std::string("18"));
        EXPECT_EQ(again.replayed_operation_count(), std::size_t{1});
    }
}
TEST(recovery_repeated_checkpoints_keep_sequences_monotonic) {
    const TempDirectory dir;
    for (std::uint64_t i = 1; i <= 5; ++i) {
        Database db(dir.file("db"));
        ASSERT_TRUE(db.open().is_ok());
        ASSERT_TRUE(db.set("round", std::to_string(i)).is_ok());
        ASSERT_TRUE(db.save().is_ok());
        ASSERT_TRUE(db.save().is_ok());
        std::uint64_t checkpoint = 0;
        std::vector<Record> records;
        ASSERT_TRUE(StorageManager(db.snapshot_path()).load(records, &checkpoint).is_ok());
        EXPECT_EQ(checkpoint, i);
    }
}
TEST(recovery_truncated_post_snapshot_record_at_every_cut) {
    const TempDirectory dir;
    Database writer(dir.file("db"));
    ASSERT_TRUE(writer.set("saved", "yes").is_ok());
    ASSERT_TRUE(writer.save().is_ok());
    ASSERT_TRUE(writer.set("complete", "yes").is_ok());
    const std::string prefix = read_file(writer.wal_path());
    ASSERT_TRUE(writer.set("incomplete", "lost").is_ok());
    const std::string full = read_file(writer.wal_path());
    for (std::size_t cut = prefix.size() + 1; cut < full.size(); ++cut) {
        ASSERT_TRUE(write_file(writer.wal_path(), full.substr(0, cut)));
        Database reader(writer.snapshot_path());
        ASSERT_TRUE(reader.open().is_ok());
        EXPECT_TRUE(reader.exists("saved"));
        EXPECT_TRUE(reader.exists("complete"));
        EXPECT_FALSE(reader.exists("incomplete"));
        EXPECT_EQ(read_file(reader.wal_path()), prefix);
        ASSERT_TRUE(reader.set("continued", "yes").is_ok());
        Database again(writer.snapshot_path());
        ASSERT_TRUE(again.open().is_ok());
        EXPECT_TRUE(again.exists("continued"));
    }
}
TEST(recovery_rejects_malformed_type_lengths_and_checksum_without_publication) {
    const TempDirectory dir;
    Database writer(dir.file("db"));
    ASSERT_TRUE(writer.set("saved", "yes").is_ok());
    ASSERT_TRUE(writer.save().is_ok());
    ASSERT_TRUE(writer.set("wal", "value").is_ok());
    const std::string valid = read_file(writer.wal_path());
    for (std::size_t offset : {std::size_t{0}, std::size_t{6}, std::size_t{16}, std::size_t{20},
                               std::size_t{24}, valid.size() - 1}) {
        std::string broken = valid;
        if (offset == 16 || offset == 20) {
            broken.replace(offset, 4, 4, static_cast<char>(0xff));
        } else {
            broken[offset] ^= 0x40;
        }
        ASSERT_TRUE(write_file(writer.wal_path(), broken));
        Database reader(writer.snapshot_path());
        EXPECT_EQ(reader.open().code(), StatusCode::CorruptData);
        EXPECT_TRUE(reader.empty());
        EXPECT_FALSE(reader.save().is_ok());
        EXPECT_EQ(read_file(reader.wal_path()), broken);
    }
}
TEST(recovery_validates_even_checkpointed_corruption) {
    const TempDirectory dir;
    WriteAheadLog log(dir.file("db.wal"));
    ASSERT_TRUE(log.append_set("a", "1").is_ok());
    ASSERT_TRUE(StorageManager(dir.file("db")).save({{"a", "1"}}, 1).is_ok());
    std::string broken = read_file(log.path());
    broken.back() ^= 1;
    ASSERT_TRUE(write_file(log.path(), broken));
    Database db(dir.file("db"));
    EXPECT_EQ(db.open().code(), StatusCode::CorruptData);
    EXPECT_EQ(read_file(log.path()), broken);
}
TEST(recovery_rejects_missing_post_checkpoint_sequence) {
    const TempDirectory dir;
    ASSERT_TRUE(StorageManager(dir.file("db")).save({}, 10).is_ok());
    WriteAheadLog log(dir.file("db.wal"));
    ASSERT_TRUE(log.reset(11).is_ok());
    ASSERT_TRUE(log.append_clear().is_ok());
    Database db(dir.file("db"));
    EXPECT_EQ(db.open().code(), StatusCode::CorruptData);
}
TEST(recovery_legacy_snapshot_and_wal_upgrade_on_save) {
    const TempDirectory dir;
    ASSERT_TRUE(StorageManager(dir.file("db")).save({{"saved", "legacy"}}).is_ok());
    WriteAheadLog log(dir.file("db.wal"));
    ASSERT_TRUE(log.append_set("new", "wal").is_ok());
    Database db(dir.file("db"));
    ASSERT_TRUE(db.open().is_ok());
    EXPECT_EQ(db.get("saved").value(), std::string("legacy"));
    EXPECT_TRUE(db.exists("new"));
    ASSERT_TRUE(db.save().is_ok());
    std::uint64_t checkpoint = 0;
    std::vector<Record> records;
    ASSERT_TRUE(StorageManager(db.snapshot_path()).load(records, &checkpoint).is_ok());
    EXPECT_EQ(checkpoint, std::uint64_t{1});
}
TEST(recovery_checkpoint_corruption_and_truncation_are_rejected) {
    const TempDirectory dir;
    const auto path = dir.file("db");
    ASSERT_TRUE(StorageManager(path).save({{"a", "value"}}, 42).is_ok());
    const std::string valid = read_file(path);
    for (std::size_t offset = 24; offset < 32; ++offset) {
        std::string broken = valid;
        broken[offset] ^= 1;
        ASSERT_TRUE(write_file(path, broken));
        std::uint64_t checkpoint = 999;
        std::vector<Record> records;
        EXPECT_EQ(StorageManager(path).load(records, &checkpoint).code(), StatusCode::CorruptData);
        EXPECT_EQ(checkpoint, std::uint64_t{0});
        EXPECT_TRUE(records.empty());
    }
    for (std::size_t cut = 0; cut < valid.size(); ++cut) {
        ASSERT_TRUE(write_file(path, valid.substr(0, cut)));
        Database db(path);
        EXPECT_EQ(db.open().code(), StatusCode::CorruptData);
    }
}
TEST(recovery_sequence_exhaustion_never_wraps) {
    const TempDirectory dir;
    ASSERT_TRUE(
        StorageManager(dir.file("db")).save({}, std::numeric_limits<std::uint64_t>::max()).is_ok());
    Database db(dir.file("db"));
    ASSERT_TRUE(db.open().is_ok());
    EXPECT_FALSE(db.set("a", "b").is_ok());
    EXPECT_TRUE(db.empty());
    EXPECT_FALSE(std::filesystem::exists(db.wal_path()));
}
TEST(recovery_ignores_uninstalled_temporary_snapshot) {
    const TempDirectory dir;
    Database db(dir.file("db"));
    mutations(db);
    ASSERT_TRUE(write_file(dir.file("db.tmp"), "partial snapshot"));
    Database reader(dir.file("db"));
    ASSERT_TRUE(reader.open().is_ok());
    expect_state(reader);
    ASSERT_TRUE(reader.save().is_ok());
    EXPECT_FALSE(std::filesystem::exists(dir.file("db.tmp")));
}
TEST(recovery_short_checkpointed_prefix_can_be_followed_by_new_operations) {
    const TempDirectory dir;
    ASSERT_TRUE(StorageManager(dir.file("db")).save({{"saved", "yes"}}, 10).is_ok());
    WriteAheadLog log(dir.file("db.wal"));
    ASSERT_TRUE(log.append_clear().is_ok());
    Database db(dir.file("db"));
    ASSERT_TRUE(db.open().is_ok());
    ASSERT_TRUE(db.set("new", "11").is_ok());
    Database reader(dir.file("db"));
    ASSERT_TRUE(reader.open().is_ok());
    EXPECT_TRUE(reader.exists("saved"));
    EXPECT_TRUE(reader.exists("new"));
}
TEST(recovery_snapshot_v2_matches_independent_binary_fixture) {
    const TempDirectory dir;
    // Version 2, one a=value record, checkpoint 42; CRC 0xb97a3f58.
    const std::string bytes = {
        0x4d, 0x49, 0x4e, 0x49, 0x44, 0x42, 0x53, 0x53, 2,    0,    0,    0,
        1,    0,    0,    0,    0,    0,    0,    0,    0x58, 0x3f, 0x7a, static_cast<char>(0xb9),
        42,   0,    0,    0,    0,    0,    0,    0,    1,    0,    0,    0,
        5,    0,    0,    0,    'a',  'v',  'a',  'l',  'u',  'e'};
    const auto path = dir.file("db");
    ASSERT_TRUE(write_file(path, bytes));
    std::uint64_t checkpoint = 0;
    std::vector<Record> records;
    ASSERT_TRUE(StorageManager(path).load(records, &checkpoint).is_ok());
    EXPECT_EQ(checkpoint, std::uint64_t{42});
    ASSERT_TRUE(records.size() == 1);
    EXPECT_EQ(records[0].key, std::string("a"));
    EXPECT_EQ(records[0].value, std::string("value"));
    ASSERT_TRUE(StorageManager(path).save(records, checkpoint).is_ok());
    EXPECT_EQ(read_file(path), bytes);
}
TEST(recovery_last_representable_sequence_is_readable_but_cannot_advance) {
    const TempDirectory dir;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    ASSERT_TRUE(StorageManager(dir.file("db")).save({}, maximum - 1).is_ok());
    WriteAheadLog log(dir.file("db.wal"));
    ASSERT_TRUE(log.reset(maximum - 1).is_ok());
    ASSERT_TRUE(log.append_set("last", "value").is_ok());
    Database db(dir.file("db"));
    ASSERT_TRUE(db.open().is_ok());
    EXPECT_EQ(db.get("last").value(), std::string("value"));
    EXPECT_FALSE(db.set("overflow", "no").is_ok());
    ASSERT_TRUE(db.save().is_ok());
    Database again(dir.file("db"));
    ASSERT_TRUE(again.open().is_ok());
    EXPECT_EQ(again.get("last").value(), std::string("value"));
    EXPECT_EQ(again.replayed_operation_count(), std::size_t{0});
}
MINIDB_TEST_MAIN()
