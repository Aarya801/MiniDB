// Tests for the in-memory Database.

#include "minidb/database.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "minidb/result.hpp"
#include "minidb/types.hpp"
#include "temp_directory.hpp"
#include "test_framework.hpp"

using minidb::Database;
using minidb::Key;
using minidb::Result;
using minidb::StatusCode;

namespace {

/// Renders the key set as a sorted, comma-joined string.
/// Comparing strings rather than vectors means a failure message shows what
/// the keys actually were instead of "<not printable>".
std::string sorted_keys(const Database& database) {
    std::vector<Key> keys = database.keys();
    std::sort(keys.begin(), keys.end());

    std::string joined;
    for (const Key& key : keys) {
        if (!joined.empty()) {
            joined += ',';
        }
        joined += key;
    }
    return joined;
}

}  // namespace

// -------------------------------------------------------------------------
// Empty database
// -------------------------------------------------------------------------

TEST(a_new_database_is_empty) {
    const Database database;
    EXPECT_TRUE(database.empty());
    EXPECT_EQ(database.size(), static_cast<std::size_t>(0));
    EXPECT_EQ(sorted_keys(database), std::string(""));
}

TEST(get_on_an_empty_database_reports_not_found) {
    const Database database;
    const Result result = database.get("missing");
    EXPECT_FALSE(result.is_ok());
    EXPECT_EQ(result.code(), StatusCode::NotFound);
}

TEST(exists_on_an_empty_database_is_false) {
    const Database database;
    EXPECT_FALSE(database.exists("missing"));
}

// -------------------------------------------------------------------------
// SET
// -------------------------------------------------------------------------

TEST(set_stores_a_new_key) {
    Database database;
    EXPECT_TRUE(database.set("name", "Aarya").is_ok());

    EXPECT_EQ(database.size(), static_cast<std::size_t>(1));
    EXPECT_FALSE(database.empty());
    EXPECT_EQ(database.get("name").value(), std::string("Aarya"));
}

TEST(set_on_an_existing_key_overwrites_without_growing) {
    Database database;
    EXPECT_TRUE(database.set("name", "Aarya").is_ok());
    EXPECT_TRUE(database.set("name", "Lalan").is_ok());

    EXPECT_EQ(database.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(database.get("name").value(), std::string("Lalan"));
}

TEST(set_stores_values_containing_spaces_verbatim) {
    Database database;
    EXPECT_TRUE(database.set("name", "Aarya Lalan").is_ok());
    EXPECT_EQ(database.get("name").value(), std::string("Aarya Lalan"));

    EXPECT_TRUE(database.set("spaced", "  padded  value  ").is_ok());
    EXPECT_EQ(database.get("spaced").value(), std::string("  padded  value  "));
}

TEST(set_accepts_an_empty_value) {
    // An empty value is a legitimate value; only an empty key is rejected.
    Database database;
    EXPECT_TRUE(database.set("blank", "").is_ok());
    EXPECT_TRUE(database.exists("blank"));
    EXPECT_EQ(database.get("blank").value(), std::string(""));
}

TEST(set_preserves_binary_values) {
    Database database;
    const std::string binary("a\0b", 3);
    EXPECT_TRUE(database.set("bin", binary).is_ok());
    EXPECT_EQ(database.get("bin").value().size(), static_cast<std::size_t>(3));
    EXPECT_EQ(database.get("bin").value(), binary);
}

TEST(set_rejects_an_empty_key) {
    Database database;
    const Result result = database.set("", "value");
    EXPECT_FALSE(result.is_ok());
    EXPECT_EQ(result.code(), StatusCode::InvalidArgument);
    EXPECT_TRUE(database.empty());
}

TEST(set_rejects_an_oversized_key) {
    Database database;
    const std::string huge_key(minidb::limits::kMaxKeySize + 1, 'k');
    const Result result = database.set(huge_key, "value");
    EXPECT_FALSE(result.is_ok());
    EXPECT_EQ(result.code(), StatusCode::KeyTooLarge);
    EXPECT_TRUE(database.empty());
}

TEST(set_accepts_a_key_of_exactly_the_maximum_size) {
    // The limit is inclusive; off-by-one here would silently reject valid data.
    Database database;
    const std::string boundary_key(minidb::limits::kMaxKeySize, 'k');
    EXPECT_TRUE(database.set(boundary_key, "value").is_ok());
    EXPECT_TRUE(database.exists(boundary_key));
}

TEST(set_rejects_an_oversized_value) {
    Database database;
    const std::string huge_value(minidb::limits::kMaxValueSize + 1, 'v');
    const Result result = database.set("key", huge_value);
    EXPECT_FALSE(result.is_ok());
    EXPECT_EQ(result.code(), StatusCode::ValueTooLarge);
    EXPECT_TRUE(database.empty());
}

// -------------------------------------------------------------------------
// GET
// -------------------------------------------------------------------------

TEST(get_returns_the_stored_value) {
    Database database;
    EXPECT_TRUE(database.set("age", "18").is_ok());

    const Result result = database.get("age");
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), std::string("18"));
}

TEST(get_reports_not_found_for_a_missing_key) {
    Database database;
    EXPECT_TRUE(database.set("present", "yes").is_ok());

    const Result result = database.get("absent");
    EXPECT_FALSE(result.is_ok());
    EXPECT_EQ(result.code(), StatusCode::NotFound);
    // The CLI prints this verbatim, so it must carry no extra message.
    EXPECT_EQ(result.to_display_string(), std::string("NOT_FOUND"));
}

TEST(get_rejects_an_empty_key) {
    const Database database;
    EXPECT_EQ(database.get("").code(), StatusCode::InvalidArgument);
}

TEST(keys_are_case_sensitive) {
    Database database;
    EXPECT_TRUE(database.set("Name", "upper").is_ok());
    EXPECT_TRUE(database.set("name", "lower").is_ok());

    EXPECT_EQ(database.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(database.get("Name").value(), std::string("upper"));
    EXPECT_EQ(database.get("name").value(), std::string("lower"));
}

// -------------------------------------------------------------------------
// DELETE
// -------------------------------------------------------------------------

TEST(remove_deletes_an_existing_key) {
    Database database;
    EXPECT_TRUE(database.set("name", "Aarya").is_ok());

    EXPECT_TRUE(database.remove("name").is_ok());
    EXPECT_FALSE(database.exists("name"));
    EXPECT_EQ(database.size(), static_cast<std::size_t>(0));
    EXPECT_EQ(database.get("name").code(), StatusCode::NotFound);
}

TEST(remove_reports_not_found_for_a_missing_key) {
    Database database;
    const Result result = database.remove("never-stored");
    EXPECT_FALSE(result.is_ok());
    EXPECT_EQ(result.code(), StatusCode::NotFound);
}

TEST(remove_leaves_other_keys_untouched) {
    Database database;
    EXPECT_TRUE(database.set("a", "1").is_ok());
    EXPECT_TRUE(database.set("b", "2").is_ok());
    EXPECT_TRUE(database.set("c", "3").is_ok());

    EXPECT_TRUE(database.remove("b").is_ok());

    EXPECT_EQ(sorted_keys(database), std::string("a,c"));
    EXPECT_EQ(database.get("a").value(), std::string("1"));
    EXPECT_EQ(database.get("c").value(), std::string("3"));
}

TEST(remove_rejects_an_empty_key) {
    Database database;
    EXPECT_EQ(database.remove("").code(), StatusCode::InvalidArgument);
}

// -------------------------------------------------------------------------
// EXISTS
// -------------------------------------------------------------------------

TEST(exists_tracks_set_and_remove) {
    Database database;
    EXPECT_FALSE(database.exists("name"));

    EXPECT_TRUE(database.set("name", "Aarya").is_ok());
    EXPECT_TRUE(database.exists("name"));

    EXPECT_TRUE(database.remove("name").is_ok());
    EXPECT_FALSE(database.exists("name"));
}

TEST(exists_is_false_for_an_invalid_key) {
    Database database;
    EXPECT_FALSE(database.exists(""));
    EXPECT_FALSE(database.exists(std::string(minidb::limits::kMaxKeySize + 1, 'k')));
}

// -------------------------------------------------------------------------
// KEYS
// -------------------------------------------------------------------------

TEST(keys_lists_every_stored_key) {
    Database database;
    EXPECT_TRUE(database.set("name", "Aarya").is_ok());
    EXPECT_TRUE(database.set("age", "18").is_ok());
    EXPECT_TRUE(database.set("city", "Pune").is_ok());

    EXPECT_EQ(database.keys().size(), static_cast<std::size_t>(3));
    EXPECT_EQ(sorted_keys(database), std::string("age,city,name"));
}

TEST(keys_does_not_repeat_an_overwritten_key) {
    Database database;
    EXPECT_TRUE(database.set("name", "first").is_ok());
    EXPECT_TRUE(database.set("name", "second").is_ok());

    EXPECT_EQ(database.keys().size(), static_cast<std::size_t>(1));
    EXPECT_EQ(sorted_keys(database), std::string("name"));
}

// -------------------------------------------------------------------------
// CLEAR
// -------------------------------------------------------------------------

TEST(clear_removes_everything) {
    Database database;
    EXPECT_TRUE(database.set("a", "1").is_ok());
    EXPECT_TRUE(database.set("b", "2").is_ok());

    EXPECT_TRUE(database.clear().is_ok());

    EXPECT_TRUE(database.empty());
    EXPECT_EQ(database.size(), static_cast<std::size_t>(0));
    EXPECT_EQ(sorted_keys(database), std::string(""));
    EXPECT_EQ(database.get("a").code(), StatusCode::NotFound);
}

TEST(clear_on_an_empty_database_is_harmless) {
    Database database;
    EXPECT_TRUE(database.clear().is_ok());
    EXPECT_TRUE(database.clear().is_ok());
    EXPECT_TRUE(database.empty());
}

TEST(the_database_is_usable_again_after_clear) {
    Database database;
    EXPECT_TRUE(database.set("a", "1").is_ok());
    EXPECT_TRUE(database.clear().is_ok());

    EXPECT_TRUE(database.set("b", "2").is_ok());
    EXPECT_EQ(database.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(database.get("b").value(), std::string("2"));
}

// -------------------------------------------------------------------------
// Repeated and mixed operations
// -------------------------------------------------------------------------

TEST(repeated_set_and_remove_cycles_leave_consistent_state) {
    Database database;
    for (int round = 0; round < 100; ++round) {
        EXPECT_TRUE(database.set("cycle", std::to_string(round)).is_ok());
        EXPECT_TRUE(database.exists("cycle"));
        EXPECT_TRUE(database.remove("cycle").is_ok());
        EXPECT_FALSE(database.exists("cycle"));
    }
    EXPECT_TRUE(database.empty());
}

TEST(many_keys_are_all_retrievable) {
    // Enough entries to force the map to rehash several times.
    Database database;
    constexpr int kCount = 1000;

    for (int index = 0; index < kCount; ++index) {
        EXPECT_TRUE(database.set("key" + std::to_string(index), std::to_string(index * 2)).is_ok());
    }
    EXPECT_EQ(database.size(), static_cast<std::size_t>(kCount));

    for (int index = 0; index < kCount; ++index) {
        const Result result = database.get("key" + std::to_string(index));
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value(), std::to_string(index * 2));
    }

    for (int index = 0; index < kCount; index += 2) {
        EXPECT_TRUE(database.remove("key" + std::to_string(index)).is_ok());
    }
    EXPECT_EQ(database.size(), static_cast<std::size_t>(kCount / 2));
    EXPECT_FALSE(database.exists("key0"));
    EXPECT_TRUE(database.exists("key1"));
}

// -------------------------------------------------------------------------
// Persistence
// -------------------------------------------------------------------------

TEST(a_default_database_is_not_persistent) {
    Database database;
    EXPECT_FALSE(database.is_persistent());
    EXPECT_FALSE(database.snapshot_exists());

    // Neither direction of persistence is available, and both say so rather
    // than pretending to have worked.
    EXPECT_EQ(database.save().code(), StatusCode::InvalidArgument);
    EXPECT_EQ(database.load().code(), StatusCode::InvalidArgument);
}

TEST(a_persistent_database_reports_its_path) {
    const minidb::testing::TempDirectory directory;
    const Database database(directory.file("db"));

    EXPECT_TRUE(database.is_persistent());
    EXPECT_EQ(database.snapshot_path(), directory.file("db"));
    EXPECT_FALSE(database.snapshot_exists());
}

TEST(entries_survive_being_saved_and_loaded_into_a_new_database) {
    // The actual promise of Milestone 3, at the Database level: what one
    // instance saves, a separate instance reads back.
    const minidb::testing::TempDirectory directory;
    const std::filesystem::path path = directory.file("db");

    {
        Database writer(path);
        EXPECT_TRUE(writer.set("name", "Aarya").is_ok());
        EXPECT_TRUE(writer.set("language", "C++").is_ok());
        EXPECT_TRUE(writer.set("note", "hello world with spaces").is_ok());
        EXPECT_TRUE(writer.save().is_ok());
    }

    Database reader(path);
    EXPECT_TRUE(reader.snapshot_exists());
    ASSERT_TRUE(reader.load().is_ok());

    EXPECT_EQ(reader.size(), static_cast<std::size_t>(3));
    EXPECT_EQ(reader.get("name").value(), std::string("Aarya"));
    EXPECT_EQ(reader.get("language").value(), std::string("C++"));
    EXPECT_EQ(reader.get("note").value(), std::string("hello world with spaces"));
    EXPECT_EQ(sorted_keys(reader), std::string("language,name,note"));
}

TEST(loading_a_database_that_was_never_saved_starts_empty) {
    const minidb::testing::TempDirectory directory;
    Database database(directory.file("never-saved"));

    // A first run is not a failure.
    EXPECT_TRUE(database.load().is_ok());
    EXPECT_TRUE(database.empty());
}

TEST(an_update_is_persisted_rather_than_duplicated) {
    const minidb::testing::TempDirectory directory;
    const std::filesystem::path path = directory.file("db");

    {
        Database writer(path);
        EXPECT_TRUE(writer.set("name", "first").is_ok());
        EXPECT_TRUE(writer.save().is_ok());
        EXPECT_TRUE(writer.set("name", "second").is_ok());
        EXPECT_TRUE(writer.save().is_ok());
    }

    Database reader(path);
    ASSERT_TRUE(reader.load().is_ok());
    EXPECT_EQ(reader.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(reader.get("name").value(), std::string("second"));
}

TEST(a_deletion_is_persisted) {
    const minidb::testing::TempDirectory directory;
    const std::filesystem::path path = directory.file("db");

    {
        Database writer(path);
        EXPECT_TRUE(writer.set("keep", "1").is_ok());
        EXPECT_TRUE(writer.set("drop", "2").is_ok());
        EXPECT_TRUE(writer.save().is_ok());
        EXPECT_TRUE(writer.remove("drop").is_ok());
        EXPECT_TRUE(writer.save().is_ok());
    }

    Database reader(path);
    ASSERT_TRUE(reader.load().is_ok());
    EXPECT_EQ(sorted_keys(reader), std::string("keep"));
    EXPECT_EQ(reader.get("drop").code(), StatusCode::NotFound);
}

TEST(a_clear_is_persisted) {
    const minidb::testing::TempDirectory directory;
    const std::filesystem::path path = directory.file("db");

    {
        Database writer(path);
        EXPECT_TRUE(writer.set("a", "1").is_ok());
        EXPECT_TRUE(writer.set("b", "2").is_ok());
        EXPECT_TRUE(writer.save().is_ok());
        EXPECT_TRUE(writer.clear().is_ok());
        EXPECT_TRUE(writer.save().is_ok());
    }

    Database reader(path);
    ASSERT_TRUE(reader.load().is_ok());
    EXPECT_TRUE(reader.empty());
}

TEST(many_entries_survive_a_save_and_load) {
    const minidb::testing::TempDirectory directory;
    const std::filesystem::path path = directory.file("db");
    constexpr int kCount = 2000;

    {
        Database writer(path);
        for (int index = 0; index < kCount; ++index) {
            EXPECT_TRUE(writer.set("key" + std::to_string(index), std::to_string(index)).is_ok());
        }
        EXPECT_TRUE(writer.save().is_ok());
    }

    Database reader(path);
    ASSERT_TRUE(reader.load().is_ok());
    EXPECT_EQ(reader.size(), static_cast<std::size_t>(kCount));

    int mismatches = 0;
    for (int index = 0; index < kCount; ++index) {
        const Result value = reader.get("key" + std::to_string(index));
        if (!value.is_ok() || value.value() != std::to_string(index)) {
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0);
}

TEST(loading_a_corrupt_snapshot_fails_and_leaves_the_database_alone) {
    // The rule that matters most: a damaged file must not quietly become an
    // empty database, because the next save would then overwrite the real
    // data with nothing.
    const minidb::testing::TempDirectory directory;
    const std::filesystem::path path = directory.file("db");

    Database database(path);
    EXPECT_TRUE(database.set("in-memory", "value").is_ok());
    EXPECT_TRUE(minidb::testing::write_file(path, "this is not a snapshot"));

    const Result loaded = database.load();
    EXPECT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.code(), StatusCode::CorruptData);

    // Untouched: the entry that was already there is still there.
    EXPECT_EQ(database.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(database.get("in-memory").value(), std::string("value"));
}

TEST(loading_twice_replaces_rather_than_merges) {
    const minidb::testing::TempDirectory directory;
    const std::filesystem::path path = directory.file("db");

    {
        Database writer(path);
        EXPECT_TRUE(writer.set("saved", "1").is_ok());
        EXPECT_TRUE(writer.save().is_ok());
    }

    Database database(path);
    ASSERT_TRUE(database.load().is_ok());
    EXPECT_TRUE(database.set("unsaved", "2").is_ok());
    ASSERT_TRUE(database.save().is_ok());
    ASSERT_TRUE(minidb::StorageManager(path).save({{"saved", "1"}}).is_ok());
    ASSERT_TRUE(database.load().is_ok());

    // load() is a replacement, not a merge: the unsaved entry is gone.
    EXPECT_EQ(sorted_keys(database), std::string("saved"));
}

MINIDB_TEST_MAIN()
