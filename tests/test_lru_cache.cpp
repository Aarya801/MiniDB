#include "minidb/database.hpp"
#include "minidb/lru_cache.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "temp_directory.hpp"
#include "test_framework.hpp"

using minidb::Database;
using minidb::LruCache;
using minidb::StatusCode;
using minidb::testing::TempDirectory;

TEST(cache_basic_put_get_missing_and_contains) {
    LruCache cache(2);
    EXPECT_EQ(cache.capacity(), std::size_t{2});
    EXPECT_EQ(cache.size(), std::size_t{0});
    EXPECT_TRUE(cache.get("missing") == nullptr);
    cache.put("a", "1");
    ASSERT_TRUE(cache.get("a") != nullptr);
    EXPECT_EQ(*cache.get("a"), std::string("1"));
    EXPECT_TRUE(cache.contains("a"));
    EXPECT_EQ(cache.size(), std::size_t{1});
}
TEST(cache_evicts_exactly_one_least_recent_item) {
    LruCache cache(3);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");
    cache.put("d", "4");
    EXPECT_FALSE(cache.contains("a"));
    EXPECT_TRUE(cache.contains("b"));
    EXPECT_TRUE(cache.contains("c"));
    EXPECT_TRUE(cache.contains("d"));
    EXPECT_EQ(cache.size(), std::size_t{3});
}
TEST(cache_get_promotes_to_mru) {
    LruCache cache(2);
    cache.put("a", "1");
    cache.put("b", "2");
    ASSERT_TRUE(cache.get("a") != nullptr);
    cache.put("c", "3");
    EXPECT_TRUE(cache.contains("a"));
    EXPECT_FALSE(cache.contains("b"));
}
TEST(cache_update_promotes_without_eviction_or_duplicate) {
    LruCache cache(2);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("a", "updated");
    EXPECT_EQ(cache.size(), std::size_t{2});
    cache.put("c", "3");
    EXPECT_FALSE(cache.contains("b"));
    ASSERT_TRUE(cache.get("a") != nullptr);
    EXPECT_EQ(*cache.get("a"), std::string("updated"));
}
TEST(cache_contains_and_misses_do_not_change_order) {
    LruCache cache(2);
    cache.put("a", "1");
    cache.put("b", "2");
    EXPECT_TRUE(cache.contains("a"));
    EXPECT_TRUE(cache.get("missing") == nullptr);
    cache.put("c", "3");
    EXPECT_FALSE(cache.contains("a"));
}
TEST(cache_zero_capacity_is_disabled) {
    LruCache cache(0);
    cache.put("a", "1");
    cache.put("a", "2");
    EXPECT_EQ(cache.size(), std::size_t{0});
    EXPECT_FALSE(cache.contains("a"));
    EXPECT_TRUE(cache.get("a") == nullptr);
    EXPECT_FALSE(cache.erase("a"));
    cache.clear();
}
TEST(cache_capacity_one_replaces_only_on_new_key) {
    LruCache cache(1);
    cache.put("a", "1");
    cache.put("a", "2");
    EXPECT_EQ(*cache.get("a"), std::string("2"));
    cache.put("b", "3");
    EXPECT_FALSE(cache.contains("a"));
    EXPECT_EQ(cache.size(), std::size_t{1});
}
TEST(cache_erase_mru_lru_and_middle) {
    LruCache cache(3);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");
    EXPECT_TRUE(cache.erase("b"));
    EXPECT_TRUE(cache.erase("c"));
    EXPECT_TRUE(cache.erase("a"));
    EXPECT_FALSE(cache.erase("a"));
    cache.put("new", "value");
    EXPECT_EQ(cache.size(), std::size_t{1});
}
TEST(cache_clear_retains_capacity_and_is_reusable) {
    LruCache cache(2);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.clear();
    cache.clear();
    EXPECT_EQ(cache.capacity(), std::size_t{2});
    EXPECT_EQ(cache.size(), std::size_t{0});
    EXPECT_TRUE(cache.get("a") == nullptr);
    cache.put("c", "3");
    EXPECT_EQ(*cache.get("c"), std::string("3"));
}
TEST(cache_binary_and_empty_strings_are_values) {
    LruCache cache(2);
    const std::string key("a\0b", 3), value("x\0y", 3);
    cache.put(key, value);
    cache.put("", "");
    EXPECT_EQ(*cache.get(key), value);
    EXPECT_EQ(*cache.get(""), std::string{});
}
TEST(cache_large_capacity_does_not_preallocate) {
    LruCache cache(std::numeric_limits<std::size_t>::max());
    cache.put("a", "1");
    EXPECT_EQ(cache.size(), std::size_t{1});
}
TEST(cache_copy_rebuilds_iterators_and_preserves_order) {
    LruCache source(2);
    source.put("a", "1");
    source.put("b", "2");
    ASSERT_TRUE(source.get("a") != nullptr);
    LruCache copy(source);
    source.clear();
    copy.put("c", "3");
    EXPECT_FALSE(copy.contains("b"));
    EXPECT_EQ(*copy.get("a"), std::string("1"));
    LruCache assigned(1);
    assigned = copy;
    copy.clear();
    EXPECT_EQ(*assigned.get("c"), std::string("3"));
    auto& alias = assigned;
    assigned = alias;
    EXPECT_EQ(assigned.size(), std::size_t{2});
}
TEST(cache_move_and_swap_keep_index_and_list_together) {
    LruCache source(2);
    source.put("a", "1");
    source.put("b", "2");
    LruCache moved(std::move(source));
    EXPECT_EQ(source.size(), std::size_t{0});
    source.put("ignored", "disabled");
    moved.put("c", "3");
    EXPECT_FALSE(moved.contains("a"));
    LruCache assigned(1);
    assigned.put("old", "gone");
    assigned = std::move(moved);
    EXPECT_EQ(*assigned.get("b"), std::string("2"));
    source.swap(assigned);
    EXPECT_EQ(*source.get("c"), std::string("3"));
}
TEST(cache_matches_simple_recency_model_over_mixed_operations) {
    LruCache cache(7);
    std::vector<std::pair<std::string, std::string>> model;  // MRU first
    unsigned state = 42;
    for (int step = 0; step < 3000; ++step) {
        state = state * 1664525U + 1013904223U;
        const std::string key = std::to_string((state >> 8) % 13);
        auto found = std::find_if(model.begin(), model.end(),
                                  [&key](const auto& item) { return item.first == key; });
        switch (state % 5) {
            case 0:
            case 1: {
                const std::string value = std::to_string(step);
                cache.put(key, value);
                if (found != model.end()) {
                    model.erase(found);
                }
                model.insert(model.begin(), {key, value});
                if (model.size() > 7) {
                    model.pop_back();
                }
                break;
            }
            case 2: {
                const auto* value = cache.get(key);
                EXPECT_EQ(value != nullptr, found != model.end());
                if (found != model.end()) {
                    ASSERT_TRUE(value != nullptr);
                    EXPECT_EQ(*value, found->second);
                    const auto item = *found;
                    model.erase(found);
                    model.insert(model.begin(), item);
                }
                break;
            }
            case 3:
                EXPECT_EQ(cache.erase(key), found != model.end());
                if (found != model.end()) {
                    model.erase(found);
                }
                break;
            default:
                if (step % 19 == 0) {
                    cache.clear();
                    model.clear();
                }
                break;
        }
        EXPECT_EQ(cache.size(), model.size());
        for (int candidate = 0; candidate < 13; ++candidate) {
            const auto name = std::to_string(candidate);
            EXPECT_EQ(cache.contains(name),
                      std::any_of(model.begin(), model.end(),
                                  [&name](const auto& item) { return item.first == name; }));
        }
    }
}
TEST(database_read_populates_cache_and_const_get_remains_supported) {
    Database db(2);
    ASSERT_TRUE(db.set("a", "1").is_ok());
    EXPECT_EQ(db.cache_size(), std::size_t{0});
    const Database& reader = db;
    EXPECT_EQ(reader.get("a").value(), std::string("1"));
    EXPECT_EQ(db.cache_size(), std::size_t{1});
    EXPECT_EQ(reader.get("a").value(), std::string("1"));
    EXPECT_EQ(db.cache_size(), std::size_t{1});
    EXPECT_EQ(reader.get("absent").code(), StatusCode::NotFound);
    EXPECT_EQ(db.cache_size(), std::size_t{1});
}
TEST(database_set_and_delete_invalidate_cached_values) {
    Database db(2);
    ASSERT_TRUE(db.set("a", "old").is_ok());
    ASSERT_TRUE(db.get("a").is_ok());
    ASSERT_TRUE(db.set("a", "new").is_ok());
    EXPECT_EQ(db.cache_size(), std::size_t{0});
    EXPECT_EQ(db.get("a").value(), std::string("new"));
    ASSERT_TRUE(db.remove("a").is_ok());
    EXPECT_EQ(db.cache_size(), std::size_t{0});
    EXPECT_EQ(db.get("a").code(), StatusCode::NotFound);
}
TEST(database_evicted_keys_remain_in_authoritative_table) {
    Database db(1);
    ASSERT_TRUE(db.set("a", "1").is_ok());
    ASSERT_TRUE(db.set("b", "2").is_ok());
    ASSERT_TRUE(db.get("a").is_ok());
    ASSERT_TRUE(db.get("b").is_ok());
    EXPECT_EQ(db.cache_size(), std::size_t{1});
    EXPECT_EQ(db.size(), std::size_t{2});
    EXPECT_EQ(db.get("a").value(), std::string("1"));
    ASSERT_TRUE(db.clear().is_ok());
    EXPECT_EQ(db.cache_size(), std::size_t{0});
    EXPECT_EQ(db.get("a").code(), StatusCode::NotFound);
}
TEST(database_zero_capacity_keeps_existing_behavior) {
    Database db(0);
    ASSERT_TRUE(db.set("a", "1").is_ok());
    EXPECT_EQ(db.get("a").value(), std::string("1"));
    EXPECT_EQ(db.cache_size(), std::size_t{0});
    EXPECT_EQ(db.cache_capacity(), std::size_t{0});
}
TEST(database_recovery_discards_old_cache_and_replays_new_value) {
    const TempDirectory dir;
    Database db(dir.file("db"), 2);
    ASSERT_TRUE(db.set("a", "snapshot").is_ok());
    ASSERT_TRUE(db.save().is_ok());
    ASSERT_TRUE(db.get("a").is_ok());
    // Simulated stopped-owner transition: no simultaneous file access.
    {
        Database writer(dir.file("db"));
        ASSERT_TRUE(writer.open().is_ok());
        ASSERT_TRUE(writer.set("a", "wal").is_ok());
    }
    ASSERT_TRUE(db.open().is_ok());
    EXPECT_EQ(db.cache_size(), std::size_t{0});
    EXPECT_EQ(db.get("a").value(), std::string("wal"));
    ASSERT_TRUE(db.save().is_ok());
    Database reopened(dir.file("db"), 0);
    ASSERT_TRUE(reopened.open().is_ok());
    EXPECT_EQ(reopened.get("a").value(), std::string("wal"));
}
TEST(database_failed_recovery_preserves_current_cached_state) {
    const TempDirectory dir;
    Database db(dir.file("db"));
    ASSERT_TRUE(db.set("a", "old").is_ok());
    ASSERT_TRUE(db.get("a").is_ok());
    ASSERT_TRUE(minidb::testing::write_file(db.wal_path(), std::string(28, 'x')));
    EXPECT_EQ(db.open().code(), StatusCode::CorruptData);
    EXPECT_EQ(db.get("a").value(), std::string("old"));
    EXPECT_EQ(db.cache_size(), std::size_t{1});
    EXPECT_FALSE(db.set("a", "new").is_ok());
    EXPECT_EQ(db.get("a").value(), std::string("old"));
}
TEST(database_failed_wal_append_does_not_invalidate_to_wrong_value) {
    const TempDirectory dir;
    Database db(dir.file("db"));
    ASSERT_TRUE(db.set("a", "old").is_ok());
    ASSERT_TRUE(db.save().is_ok());
    ASSERT_TRUE(db.get("a").is_ok());
    std::filesystem::remove(db.wal_path());
    std::filesystem::create_directory(db.wal_path());
    EXPECT_FALSE(db.set("a", "new").is_ok());
    EXPECT_EQ(db.get("a").value(), std::string("old"));
    EXPECT_FALSE(db.remove("a").is_ok());
    EXPECT_EQ(db.get("a").value(), std::string("old"));
}
TEST(database_copy_has_independent_cache_and_values) {
    Database source(2);
    ASSERT_TRUE(source.set("a", "old").is_ok());
    ASSERT_TRUE(source.get("a").is_ok());
    Database copy(source);
    ASSERT_TRUE(source.clear().is_ok());
    EXPECT_EQ(copy.get("a").value(), std::string("old"));
    ASSERT_TRUE(copy.set("a", "new").is_ok());
    EXPECT_EQ(copy.get("a").value(), std::string("new"));
    EXPECT_EQ(source.get("a").code(), StatusCode::NotFound);
}
MINIDB_TEST_MAIN()
