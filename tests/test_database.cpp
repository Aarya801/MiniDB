// Tests for the in-memory Database.

#include "minidb/database.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "minidb/result.hpp"
#include "minidb/types.hpp"
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

    database.clear();

    EXPECT_TRUE(database.empty());
    EXPECT_EQ(database.size(), static_cast<std::size_t>(0));
    EXPECT_EQ(sorted_keys(database), std::string(""));
    EXPECT_EQ(database.get("a").code(), StatusCode::NotFound);
}

TEST(clear_on_an_empty_database_is_harmless) {
    Database database;
    database.clear();
    database.clear();
    EXPECT_TRUE(database.empty());
}

TEST(the_database_is_usable_again_after_clear) {
    Database database;
    EXPECT_TRUE(database.set("a", "1").is_ok());
    database.clear();

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

MINIDB_TEST_MAIN()
