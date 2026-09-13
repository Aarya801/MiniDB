#include "minidb/transaction.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "minidb/database.hpp"
#include "minidb/result.hpp"
#include "minidb/wal.hpp"
#include "temp_directory.hpp"
#include "test_framework.hpp"

using minidb::Database;
using minidb::Result;
using minidb::StatusCode;
using minidb::Transaction;
using minidb::WalRecord;
using minidb::WriteAheadLog;
using minidb::testing::read_file;
using minidb::testing::TempDirectory;
using minidb::testing::write_file;

TEST(begin_activates_an_empty_transaction) {
    Database database;
    Transaction transaction(database);
    EXPECT_FALSE(transaction.active());
    EXPECT_TRUE(transaction.begin().is_ok());
    EXPECT_TRUE(transaction.active());
    EXPECT_EQ(transaction.change_count(), std::size_t{0});
}

TEST(begin_while_active_is_rejected_without_losing_changes) {
    Database database;
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    ASSERT_TRUE(transaction.set("a", "1").is_ok());
    const Result repeated = transaction.begin();
    EXPECT_EQ(repeated.code(), StatusCode::InvalidTransactionState);
    EXPECT_TRUE(transaction.active());
    EXPECT_EQ(transaction.get("a").value(), std::string("1"));
}

TEST(set_and_get_use_transaction_local_state) {
    Database database;
    ASSERT_TRUE(database.set("name", "old").is_ok());
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    ASSERT_TRUE(transaction.set("name", "Aarya").is_ok());
    ASSERT_TRUE(transaction.set("language", "C++").is_ok());
    EXPECT_EQ(transaction.get("name").value(), std::string("Aarya"));
    EXPECT_EQ(transaction.get("language").value(), std::string("C++"));
    EXPECT_EQ(database.get("name").value(), std::string("old"));
    EXPECT_EQ(database.get("language").code(), StatusCode::NotFound);
}

TEST(delete_is_visible_locally_and_not_before_commit) {
    Database database;
    ASSERT_TRUE(database.set("a", "1").is_ok());
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    ASSERT_TRUE(transaction.remove("a").is_ok());
    EXPECT_EQ(transaction.get("a").code(), StatusCode::NotFound);
    EXPECT_FALSE(transaction.exists("a"));
    EXPECT_TRUE(database.exists("a"));
}

TEST(delete_of_missing_key_reports_not_found_and_stays_active) {
    Database database;
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    EXPECT_EQ(transaction.remove("missing").code(), StatusCode::NotFound);
    EXPECT_TRUE(transaction.active());
    EXPECT_EQ(transaction.change_count(), std::size_t{0});
}

TEST(commit_applies_multiple_changes_and_becomes_inactive) {
    Database database;
    ASSERT_TRUE(database.set("drop", "old").is_ok());
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    ASSERT_TRUE(transaction.set("name", "Aarya").is_ok());
    ASSERT_TRUE(transaction.set("language", "C++").is_ok());
    ASSERT_TRUE(transaction.remove("drop").is_ok());
    ASSERT_TRUE(transaction.commit().is_ok());
    EXPECT_FALSE(transaction.active());
    EXPECT_EQ(database.get("name").value(), std::string("Aarya"));
    EXPECT_EQ(database.get("language").value(), std::string("C++"));
    EXPECT_FALSE(database.exists("drop"));
}

TEST(rollback_discards_multiple_changes) {
    Database database;
    ASSERT_TRUE(database.set("name", "old").is_ok());
    ASSERT_TRUE(database.set("keep", "yes").is_ok());
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    ASSERT_TRUE(transaction.set("name", "Temporary").is_ok());
    ASSERT_TRUE(transaction.remove("keep").is_ok());
    ASSERT_TRUE(transaction.set("new", "discard").is_ok());
    ASSERT_TRUE(transaction.rollback().is_ok());
    EXPECT_FALSE(transaction.active());
    EXPECT_EQ(database.get("name").value(), std::string("old"));
    EXPECT_EQ(database.get("keep").value(), std::string("yes"));
    EXPECT_FALSE(database.exists("new"));
}

TEST(overwriting_the_same_key_commits_only_final_value) {
    Database database;
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    ASSERT_TRUE(transaction.set("key", "one").is_ok());
    ASSERT_TRUE(transaction.set("key", "two").is_ok());
    ASSERT_TRUE(transaction.set("key", "three").is_ok());
    EXPECT_EQ(transaction.change_count(), std::size_t{1});
    ASSERT_TRUE(transaction.commit().is_ok());
    EXPECT_EQ(database.get("key").value(), std::string("three"));
}

TEST(delete_then_recreate_commits_the_new_value) {
    Database database;
    ASSERT_TRUE(database.set("key", "old").is_ok());
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    ASSERT_TRUE(transaction.remove("key").is_ok());
    ASSERT_TRUE(transaction.set("key", "new").is_ok());
    ASSERT_TRUE(transaction.commit().is_ok());
    EXPECT_EQ(database.get("key").value(), std::string("new"));
}

TEST(set_then_delete_of_new_key_leaves_it_absent) {
    Database database;
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    ASSERT_TRUE(transaction.set("temporary", "value").is_ok());
    ASSERT_TRUE(transaction.remove("temporary").is_ok());
    ASSERT_TRUE(transaction.commit().is_ok());
    EXPECT_FALSE(database.exists("temporary"));
}

TEST(empty_commit_and_rollback_are_valid) {
    Database database;
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    ASSERT_TRUE(transaction.commit().is_ok());
    EXPECT_FALSE(transaction.active());
    ASSERT_TRUE(transaction.begin().is_ok());
    ASSERT_TRUE(transaction.rollback().is_ok());
    EXPECT_FALSE(transaction.active());
}

TEST(transaction_commands_without_begin_are_rejected) {
    Database database;
    Transaction transaction(database);
    EXPECT_EQ(transaction.set("a", "1").code(), StatusCode::InvalidTransactionState);
    EXPECT_EQ(transaction.get("a").code(), StatusCode::InvalidTransactionState);
    EXPECT_EQ(transaction.remove("a").code(), StatusCode::InvalidTransactionState);
    EXPECT_EQ(transaction.clear().code(), StatusCode::InvalidTransactionState);
    EXPECT_EQ(transaction.commit().code(), StatusCode::InvalidTransactionState);
    EXPECT_EQ(transaction.rollback().code(), StatusCode::InvalidTransactionState);
}

TEST(invalid_local_mutations_do_not_end_the_transaction) {
    Database database;
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    EXPECT_EQ(transaction.set("", "value").code(), StatusCode::InvalidArgument);
    EXPECT_EQ(transaction.set("key", std::string(minidb::limits::kMaxValueSize + 1, 'v')).code(),
              StatusCode::ValueTooLarge);
    EXPECT_TRUE(transaction.active());
    ASSERT_TRUE(transaction.set("valid", "yes").is_ok());
    ASSERT_TRUE(transaction.commit().is_ok());
    EXPECT_TRUE(database.exists("valid"));
}

TEST(repeated_transactions_are_independent) {
    Database database;
    Transaction transaction(database);
    for (int round = 0; round < 20; ++round) {
        ASSERT_TRUE(transaction.begin().is_ok());
        ASSERT_TRUE(transaction.set("round", std::to_string(round)).is_ok());
        if (round % 2 == 0) {
            ASSERT_TRUE(transaction.commit().is_ok());
            EXPECT_EQ(database.get("round").value(), std::to_string(round));
        } else {
            ASSERT_TRUE(transaction.rollback().is_ok());
            EXPECT_EQ(database.get("round").value(), std::to_string(round - 1));
        }
    }
}

TEST(keys_exists_and_clear_follow_the_local_view) {
    Database database;
    ASSERT_TRUE(database.set("a", "1").is_ok());
    ASSERT_TRUE(database.set("b", "2").is_ok());
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    ASSERT_TRUE(transaction.remove("a").is_ok());
    ASSERT_TRUE(transaction.set("c", "3").is_ok());
    EXPECT_FALSE(transaction.exists("a"));
    EXPECT_TRUE(transaction.exists("b"));
    EXPECT_TRUE(transaction.exists("c"));
    EXPECT_EQ(transaction.keys().size(), std::size_t{2});
    ASSERT_TRUE(transaction.clear().is_ok());
    EXPECT_TRUE(transaction.keys().empty());
    ASSERT_TRUE(transaction.commit().is_ok());
    EXPECT_TRUE(database.empty());
}

TEST(commit_is_one_wal_record_and_recovers_all_changes) {
    const TempDirectory directory;
    const auto path = directory.file("db");
    Database database(path);
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    ASSERT_TRUE(transaction.set("name", "Aarya").is_ok());
    ASSERT_TRUE(transaction.set("language", "C++").is_ok());
    ASSERT_TRUE(transaction.commit().is_ok());

    WriteAheadLog wal(database.wal_path());
    std::vector<WalRecord> records;
    ASSERT_TRUE(wal.replay(records).is_ok());
    ASSERT_TRUE(records.size() == 2);
    EXPECT_EQ(records[0].sequence, std::uint64_t{1});
    EXPECT_EQ(records[1].sequence, std::uint64_t{1});

    Database recovered(path);
    ASSERT_TRUE(recovered.open().is_ok());
    EXPECT_EQ(recovered.get("name").value(), std::string("Aarya"));
    EXPECT_EQ(recovered.get("language").value(), std::string("C++"));
    EXPECT_EQ(recovered.replayed_operation_count(), std::size_t{2});
}

TEST(commit_persists_across_snapshot_and_restart) {
    const TempDirectory directory;
    const auto path = directory.file("db");
    {
        Database database(path);
        Transaction transaction(database);
        ASSERT_TRUE(transaction.begin().is_ok());
        ASSERT_TRUE(transaction.set("name", "Aarya").is_ok());
        ASSERT_TRUE(transaction.set("language", "C++").is_ok());
        ASSERT_TRUE(transaction.commit().is_ok());
        ASSERT_TRUE(database.save().is_ok());
    }
    Database recovered(path);
    ASSERT_TRUE(recovered.open().is_ok());
    EXPECT_EQ(recovered.get("name").value(), std::string("Aarya"));
    EXPECT_EQ(recovered.get("language").value(), std::string("C++"));
    EXPECT_EQ(recovered.replayed_operation_count(), std::size_t{0});
}

TEST(checkpointed_transaction_wal_record_is_validated_but_not_replayed) {
    const TempDirectory directory;
    const auto path = directory.file("db");
    Database database(path);
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    ASSERT_TRUE(transaction.set("name", "Aarya").is_ok());
    ASSERT_TRUE(transaction.set("language", "C++").is_ok());
    ASSERT_TRUE(transaction.commit().is_ok());
    const std::string committed_wal = read_file(database.wal_path());
    ASSERT_TRUE(!committed_wal.empty());

    // Simulate a crash after the new snapshot was installed but before WAL
    // reset. The checkpoint says sequence 1 is already represented.
    const std::vector<minidb::Record> snapshot = {{"name", "Aarya"},
                                                   {"language", "C++"}};
    ASSERT_TRUE(minidb::StorageManager(path).save(snapshot, 1).is_ok());
    EXPECT_EQ(read_file(database.wal_path()), committed_wal);

    Database recovered(path);
    ASSERT_TRUE(recovered.open().is_ok());
    EXPECT_EQ(recovered.get("name").value(), std::string("Aarya"));
    EXPECT_EQ(recovered.get("language").value(), std::string("C++"));
    EXPECT_EQ(recovered.replayed_operation_count(), std::size_t{0});
}

TEST(rollback_never_writes_the_wal_or_persisted_state) {
    const TempDirectory directory;
    const auto path = directory.file("db");
    Database database(path);
    ASSERT_TRUE(database.set("name", "old").is_ok());
    ASSERT_TRUE(database.save().is_ok());
    const std::string wal_before = read_file(database.wal_path());
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    ASSERT_TRUE(transaction.set("name", "Temporary").is_ok());
    ASSERT_TRUE(transaction.set("new", "discard").is_ok());
    ASSERT_TRUE(transaction.rollback().is_ok());
    EXPECT_EQ(read_file(database.wal_path()), wal_before);

    Database recovered(path);
    ASSERT_TRUE(recovered.open().is_ok());
    EXPECT_EQ(recovered.get("name").value(), std::string("old"));
    EXPECT_FALSE(recovered.exists("new"));
}

TEST(commit_clears_stale_cache_only_after_success) {
    Database database(4);
    ASSERT_TRUE(database.set("name", "old").is_ok());
    ASSERT_TRUE(database.get("name").is_ok());
    EXPECT_EQ(database.cache_size(), std::size_t{1});
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    ASSERT_TRUE(transaction.set("name", "new").is_ok());
    EXPECT_EQ(database.get("name").value(), std::string("old"));
    EXPECT_EQ(database.cache_size(), std::size_t{1});
    ASSERT_TRUE(transaction.commit().is_ok());
    EXPECT_EQ(database.cache_size(), std::size_t{0});
    EXPECT_EQ(database.get("name").value(), std::string("new"));
}

TEST(rollback_leaves_the_valid_main_cache_untouched) {
    Database database(4);
    ASSERT_TRUE(database.set("name", "old").is_ok());
    ASSERT_TRUE(database.get("name").is_ok());
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    ASSERT_TRUE(transaction.set("name", "temporary").is_ok());
    ASSERT_TRUE(transaction.rollback().is_ok());
    EXPECT_EQ(database.cache_size(), std::size_t{1});
    EXPECT_EQ(database.get("name").value(), std::string("old"));
}

TEST(a_torn_transaction_wal_record_recovers_none_of_its_changes) {
    const TempDirectory directory;
    const auto path = directory.file("db");
    Database database(path);
    ASSERT_TRUE(database.set("base", "saved").is_ok());
    ASSERT_TRUE(database.save().is_ok());
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    ASSERT_TRUE(transaction.set("a", "1").is_ok());
    ASSERT_TRUE(transaction.set("b", "2").is_ok());
    ASSERT_TRUE(transaction.commit().is_ok());
    const std::string full = read_file(database.wal_path());
    ASSERT_TRUE(full.size() > WriteAheadLog::kRecordHeaderSize);

    for (std::size_t cut = 1; cut < full.size(); ++cut) {
        ASSERT_TRUE(write_file(database.wal_path(), full.substr(0, cut)));
        Database recovered(path);
        ASSERT_TRUE(recovered.open().is_ok());
        EXPECT_EQ(recovered.get("base").value(), std::string("saved"));
        EXPECT_FALSE(recovered.exists("a"));
        EXPECT_FALSE(recovered.exists("b"));
        EXPECT_EQ(std::filesystem::file_size(recovered.wal_path()), std::uintmax_t{0});
    }
}

TEST(failed_commit_is_inactive_and_does_not_change_database) {
    const TempDirectory directory;
    const auto path = directory.file("db");
    Database database(path);
    ASSERT_TRUE(database.set("base", "old").is_ok());
    ASSERT_TRUE(database.save().is_ok());
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    const std::string large(600U * 1024U, 'v');
    ASSERT_TRUE(transaction.set("a", large).is_ok());
    ASSERT_TRUE(transaction.set("b", large).is_ok());
    EXPECT_EQ(transaction.commit().code(), StatusCode::InvalidArgument);
    EXPECT_FALSE(transaction.active());
    EXPECT_EQ(database.get("base").value(), std::string("old"));
    EXPECT_FALSE(database.exists("a"));
    EXPECT_FALSE(database.exists("b"));
    EXPECT_EQ(std::filesystem::file_size(database.wal_path()), std::uintmax_t{0});
}

TEST(wal_io_failure_keeps_authoritative_state_and_cache_consistent) {
    const TempDirectory directory;
    const auto path = directory.file("db");
    Database database(path, 4);
    ASSERT_TRUE(database.set("name", "old").is_ok());
    ASSERT_TRUE(database.save().is_ok());
    ASSERT_TRUE(database.get("name").is_ok());
    EXPECT_EQ(database.cache_size(), std::size_t{1});

    ASSERT_TRUE(std::filesystem::remove(database.wal_path()));
    ASSERT_TRUE(std::filesystem::create_directory(database.wal_path()));
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());
    ASSERT_TRUE(transaction.set("name", "new").is_ok());
    EXPECT_EQ(transaction.commit().code(), StatusCode::IoError);
    EXPECT_FALSE(transaction.active());
    EXPECT_EQ(database.get("name").value(), std::string("old"));
    EXPECT_EQ(database.cache_size(), std::size_t{1});

    ASSERT_TRUE(std::filesystem::remove(database.wal_path()));
    ASSERT_TRUE(write_file(database.wal_path(), ""));
    ASSERT_TRUE(database.open().is_ok());
    EXPECT_EQ(database.get("name").value(), std::string("old"));
}

TEST(begin_on_corrupt_persistent_database_fails_without_activation) {
    const TempDirectory directory;
    const auto path = directory.file("db");
    ASSERT_TRUE(write_file(path, "not a snapshot"));
    Database database(path);
    Transaction transaction(database);
    EXPECT_EQ(transaction.begin().code(), StatusCode::CorruptData);
    EXPECT_FALSE(transaction.active());
}

MINIDB_TEST_MAIN()
