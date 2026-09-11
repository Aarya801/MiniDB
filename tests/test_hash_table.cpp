// Tests for the custom hash table.
//
// Collision behaviour is tested by supplying a deliberately bad hasher as the
// template argument, not by hoping that two real keys happen to collide. The
// production table is untouched by this: Hash is a normal template parameter
// with std::hash as its default, so the tests exercise the same chaining code
// the database uses, just with every key steered into one bucket.

#include "minidb/hash_table.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "test_framework.hpp"

using minidb::HashTable;

namespace {

/// Sends every key to the same bucket. Turns the table into a single chain,
/// which is the worst case separate chaining has to survive.
struct AlwaysCollide {
    [[nodiscard]] std::size_t operator()(const std::string&) const noexcept { return 0; }
};

/// Hashes to the key's length, so keys collide in controlled groups: every
/// key of the same length shares a bucket, different lengths generally do not.
struct HashByLength {
    [[nodiscard]] std::size_t operator()(const std::string& key) const noexcept {
        return key.size();
    }
};

/// Counts live instances, so a test can prove the table destroys exactly as
/// many values as it creates. Every constructor increments and the destructor
/// decrements, including the move constructor -- a moved-from object still
/// exists and still has to be destroyed.
struct Counted {
    static inline int live = 0;

    int value = 0;

    Counted() { ++live; }
    explicit Counted(int initial) : value(initial) { ++live; }
    Counted(const Counted& other) : value(other.value) { ++live; }
    Counted(Counted&& other) noexcept : value(other.value) { ++live; }
    Counted& operator=(const Counted& other) = default;
    Counted& operator=(Counted&& other) noexcept = default;
    ~Counted() { --live; }

    [[nodiscard]] bool operator==(const Counted& other) const noexcept {
        return value == other.value;
    }
};

using CollidingTable = HashTable<std::string, int, AlwaysCollide>;
using LengthTable = HashTable<std::string, int, HashByLength>;
using StringTable = HashTable<std::string, int>;
using IntTable = HashTable<int, std::string>;

/// Sorted, comma-joined keys. Comparing a string gives readable failures.
template<typename Table>
std::string sorted_keys(const Table& table) {
    std::vector<std::string> keys;
    table.for_each([&keys](const std::string& key, const auto&) { keys.push_back(key); });
    std::sort(keys.begin(), keys.end());

    std::string joined;
    for (const std::string& key : keys) {
        if (!joined.empty()) {
            joined += ',';
        }
        joined += key;
    }
    return joined;
}

}  // namespace

// -------------------------------------------------------------------------
// Empty table
// -------------------------------------------------------------------------

TEST(a_new_table_is_empty_but_has_buckets) {
    const StringTable table;
    EXPECT_TRUE(table.empty());
    EXPECT_EQ(table.size(), static_cast<std::size_t>(0));
    EXPECT_TRUE(table.bucket_count() > 0);
    EXPECT_EQ(table.load_factor(), 0.0F);
}

TEST(lookups_on_an_empty_table_find_nothing) {
    StringTable table;
    EXPECT_TRUE(table.find("missing") == nullptr);
    EXPECT_FALSE(table.contains("missing"));
    EXPECT_FALSE(table.erase("missing"));
    EXPECT_EQ(table.size(), static_cast<std::size_t>(0));
}

TEST(clearing_an_empty_table_is_harmless) {
    StringTable table;
    table.clear();
    table.clear();
    EXPECT_TRUE(table.empty());
}

// -------------------------------------------------------------------------
// Insert, find, update
// -------------------------------------------------------------------------

TEST(insert_stores_a_value) {
    StringTable table;
    EXPECT_TRUE(table.insert_or_assign("one", 1));

    EXPECT_EQ(table.size(), static_cast<std::size_t>(1));
    EXPECT_FALSE(table.empty());
    ASSERT_TRUE(table.find("one") != nullptr);
    EXPECT_EQ(*table.find("one"), 1);
    EXPECT_TRUE(table.contains("one"));
}

TEST(insert_reports_whether_the_entry_was_new) {
    StringTable table;
    EXPECT_TRUE(table.insert_or_assign("k", 1));   // created
    EXPECT_FALSE(table.insert_or_assign("k", 2));  // replaced
}

TEST(inserting_a_duplicate_key_updates_without_growing) {
    StringTable table;
    table.insert_or_assign("k", 1);
    table.insert_or_assign("k", 2);
    table.insert_or_assign("k", 3);

    EXPECT_EQ(table.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(*table.find("k"), 3);
    EXPECT_EQ(sorted_keys(table), std::string("k"));
}

TEST(distinct_keys_are_stored_separately) {
    StringTable table;
    table.insert_or_assign("a", 1);
    table.insert_or_assign("b", 2);
    table.insert_or_assign("c", 3);

    EXPECT_EQ(table.size(), static_cast<std::size_t>(3));
    EXPECT_EQ(*table.find("a"), 1);
    EXPECT_EQ(*table.find("b"), 2);
    EXPECT_EQ(*table.find("c"), 3);
    EXPECT_EQ(sorted_keys(table), std::string("a,b,c"));
}

TEST(find_returns_a_pointer_that_can_modify_the_value) {
    StringTable table;
    table.insert_or_assign("k", 1);

    int* value = table.find("k");
    ASSERT_TRUE(value != nullptr);
    *value = 99;

    EXPECT_EQ(*table.find("k"), 99);
    EXPECT_EQ(table.size(), static_cast<std::size_t>(1));
}

TEST(const_find_works_on_a_const_table) {
    StringTable table;
    table.insert_or_assign("k", 7);

    const StringTable& constant = table;
    ASSERT_TRUE(constant.find("k") != nullptr);
    EXPECT_EQ(*constant.find("k"), 7);
    EXPECT_TRUE(constant.find("absent") == nullptr);
}

// -------------------------------------------------------------------------
// Erase
// -------------------------------------------------------------------------

TEST(erase_removes_an_entry) {
    StringTable table;
    table.insert_or_assign("k", 1);

    EXPECT_TRUE(table.erase("k"));
    EXPECT_EQ(table.size(), static_cast<std::size_t>(0));
    EXPECT_FALSE(table.contains("k"));
    EXPECT_TRUE(table.find("k") == nullptr);
}

TEST(erasing_a_missing_key_reports_false_and_changes_nothing) {
    StringTable table;
    table.insert_or_assign("present", 1);

    EXPECT_FALSE(table.erase("absent"));
    EXPECT_EQ(table.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(table.contains("present"));
}

TEST(erasing_twice_only_succeeds_once) {
    StringTable table;
    table.insert_or_assign("k", 1);

    EXPECT_TRUE(table.erase("k"));
    EXPECT_FALSE(table.erase("k"));
    EXPECT_EQ(table.size(), static_cast<std::size_t>(0));
}

TEST(erase_leaves_the_other_entries_alone) {
    StringTable table;
    for (int index = 0; index < 20; ++index) {
        table.insert_or_assign("key" + std::to_string(index), index);
    }

    EXPECT_TRUE(table.erase("key7"));

    EXPECT_EQ(table.size(), static_cast<std::size_t>(19));
    EXPECT_FALSE(table.contains("key7"));
    for (int index = 0; index < 20; ++index) {
        if (index != 7) {
            ASSERT_TRUE(table.contains("key" + std::to_string(index)));
            EXPECT_EQ(*table.find("key" + std::to_string(index)), index);
        }
    }
}

// -------------------------------------------------------------------------
// Collisions -- separate chaining
// -------------------------------------------------------------------------

TEST(colliding_keys_share_a_bucket_and_all_survive) {
    CollidingTable table;
    for (int index = 0; index < 50; ++index) {
        table.insert_or_assign("key" + std::to_string(index), index);
    }

    // Every key really is in one bucket: this is a chain of 50 nodes, not
    // 50 keys that happen to be spread around.
    EXPECT_EQ(table.size(), static_cast<std::size_t>(50));
    EXPECT_EQ(table.bucket_index("key0"), table.bucket_index("key49"));
    EXPECT_EQ(table.bucket_size(table.bucket_index("key0")), static_cast<std::size_t>(50));

    for (int index = 0; index < 50; ++index) {
        ASSERT_TRUE(table.contains("key" + std::to_string(index)));
        EXPECT_EQ(*table.find("key" + std::to_string(index)), index);
    }
}

TEST(a_duplicate_key_in_a_full_chain_updates_rather_than_chaining) {
    CollidingTable table;
    for (int index = 0; index < 10; ++index) {
        table.insert_or_assign("key" + std::to_string(index), index);
    }
    table.insert_or_assign("key5", 500);

    EXPECT_EQ(table.size(), static_cast<std::size_t>(10));
    EXPECT_EQ(table.bucket_size(table.bucket_index("key5")), static_cast<std::size_t>(10));
    EXPECT_EQ(*table.find("key5"), 500);
}

TEST(erasing_from_the_head_of_a_chain_keeps_the_rest) {
    CollidingTable table;
    for (int index = 0; index < 10; ++index) {
        table.insert_or_assign("key" + std::to_string(index), index);
    }

    // Inserts push onto the front, so key9 is the head node.
    EXPECT_TRUE(table.erase("key9"));

    EXPECT_EQ(table.size(), static_cast<std::size_t>(9));
    EXPECT_FALSE(table.contains("key9"));
    for (int index = 0; index < 9; ++index) {
        EXPECT_TRUE(table.contains("key" + std::to_string(index)));
    }
}

TEST(erasing_from_the_middle_of_a_chain_relinks_it) {
    CollidingTable table;
    for (int index = 0; index < 10; ++index) {
        table.insert_or_assign("key" + std::to_string(index), index);
    }

    EXPECT_TRUE(table.erase("key5"));

    EXPECT_EQ(table.size(), static_cast<std::size_t>(9));
    EXPECT_EQ(table.bucket_size(table.bucket_index("key0")), static_cast<std::size_t>(9));
    // A broken relink would lose everything past the removed node.
    for (int index = 0; index < 10; ++index) {
        if (index != 5) {
            ASSERT_TRUE(table.contains("key" + std::to_string(index)));
            EXPECT_EQ(*table.find("key" + std::to_string(index)), index);
        }
    }
}

TEST(erasing_from_the_tail_of_a_chain_keeps_the_rest) {
    CollidingTable table;
    for (int index = 0; index < 10; ++index) {
        table.insert_or_assign("key" + std::to_string(index), index);
    }

    EXPECT_TRUE(table.erase("key0"));  // inserted first, so it is the tail

    EXPECT_EQ(table.size(), static_cast<std::size_t>(9));
    EXPECT_FALSE(table.contains("key0"));
    EXPECT_EQ(sorted_keys(table), std::string("key1,key2,key3,key4,key5,key6,key7,key8,key9"));
}

TEST(an_entire_chain_can_be_erased_one_node_at_a_time) {
    CollidingTable table;
    for (int index = 0; index < 30; ++index) {
        table.insert_or_assign("key" + std::to_string(index), index);
    }
    for (int index = 0; index < 30; ++index) {
        EXPECT_TRUE(table.erase("key" + std::to_string(index)));
    }

    EXPECT_TRUE(table.empty());
    EXPECT_EQ(table.bucket_size(0), static_cast<std::size_t>(0));
}

TEST(partial_collisions_group_keys_correctly) {
    LengthTable table;
    table.insert_or_assign("aa", 2);
    table.insert_or_assign("bb", 20);
    table.insert_or_assign("ccc", 3);

    // Same length means same hash means same bucket.
    EXPECT_EQ(table.bucket_index("aa"), table.bucket_index("bb"));
    EXPECT_TRUE(table.bucket_index("aa") != table.bucket_index("ccc"));

    EXPECT_EQ(*table.find("aa"), 2);
    EXPECT_EQ(*table.find("bb"), 20);
    EXPECT_EQ(*table.find("ccc"), 3);
}

TEST(a_missing_key_that_collides_with_a_present_one_is_not_found) {
    // The chain walk must compare keys, not just land on the right bucket.
    CollidingTable table;
    table.insert_or_assign("present", 1);

    EXPECT_EQ(table.bucket_index("absent"), table.bucket_index("present"));
    EXPECT_FALSE(table.contains("absent"));
    EXPECT_TRUE(table.find("absent") == nullptr);
}

// -------------------------------------------------------------------------
// Resizing and rehashing
// -------------------------------------------------------------------------

TEST(the_table_grows_once_the_load_factor_is_exceeded) {
    StringTable table;
    const std::size_t initial_buckets = table.bucket_count();

    for (int index = 0; index < 200; ++index) {
        table.insert_or_assign("key" + std::to_string(index), index);
    }

    EXPECT_TRUE(table.bucket_count() > initial_buckets);
    EXPECT_EQ(table.size(), static_cast<std::size_t>(200));
}

TEST(the_load_factor_never_exceeds_the_threshold) {
    StringTable table;
    for (int index = 0; index < 500; ++index) {
        table.insert_or_assign("key" + std::to_string(index), index);
        EXPECT_TRUE(table.load_factor() <= StringTable::max_load_factor());
    }
}

TEST(every_entry_survives_repeated_rehashing) {
    // 2000 entries forces several growth steps from the initial 17 buckets.
    StringTable table;
    constexpr int kCount = 2000;

    for (int index = 0; index < kCount; ++index) {
        table.insert_or_assign("key" + std::to_string(index), index * 3);
    }

    EXPECT_EQ(table.size(), static_cast<std::size_t>(kCount));
    for (int index = 0; index < kCount; ++index) {
        const int* value = table.find("key" + std::to_string(index));
        ASSERT_TRUE(value != nullptr);
        EXPECT_EQ(*value, index * 3);
    }
}

TEST(an_explicit_rehash_preserves_every_entry) {
    StringTable table;
    for (int index = 0; index < 100; ++index) {
        table.insert_or_assign("key" + std::to_string(index), index);
    }
    const std::string before = sorted_keys(table);

    table.rehash(1000);

    EXPECT_TRUE(table.bucket_count() >= 1000);
    EXPECT_EQ(table.size(), static_cast<std::size_t>(100));
    EXPECT_EQ(sorted_keys(table), before);
    for (int index = 0; index < 100; ++index) {
        EXPECT_EQ(*table.find("key" + std::to_string(index)), index);
    }
}

TEST(rehash_never_shrinks_below_the_load_factor) {
    // Asking for one bucket must not squeeze 1000 entries into 17 buckets:
    // the request is raised to whatever keeps the load factor in bounds.
    StringTable table;
    for (int index = 0; index < 1000; ++index) {
        table.insert_or_assign("key" + std::to_string(index), index);
    }

    table.rehash(1);

    EXPECT_TRUE(table.load_factor() <= StringTable::max_load_factor());
    EXPECT_EQ(table.size(), static_cast<std::size_t>(1000));
    for (int index = 0; index < 1000; ++index) {
        const int* value = table.find("key" + std::to_string(index));
        ASSERT_TRUE(value != nullptr);
        EXPECT_EQ(*value, index);
    }
}

TEST(a_pointer_from_find_survives_a_rehash) {
    // Rehashing relinks nodes instead of recreating them, so the value a
    // caller is holding does not move.
    StringTable table;
    table.insert_or_assign("stable", 1);

    int* value = table.find("stable");
    ASSERT_TRUE(value != nullptr);

    table.rehash(5000);

    EXPECT_EQ(*value, 1);
    *value = 42;
    EXPECT_EQ(*table.find("stable"), 42);
}

TEST(bucket_counts_are_prime) {
    // A prime modulus is what keeps a weak hash from collapsing into a few
    // buckets, so the growth sequence must actually deliver primes.
    const auto is_prime = [](std::size_t value) {
        if (value < 2) {
            return false;
        }
        for (std::size_t divisor = 2; divisor * divisor <= value; ++divisor) {
            if (value % divisor == 0) {
                return false;
            }
        }
        return true;
    };

    StringTable table;
    EXPECT_TRUE(is_prime(table.bucket_count()));
    for (int index = 0; index < 1000; ++index) {
        table.insert_or_assign("key" + std::to_string(index), index);
        EXPECT_TRUE(is_prime(table.bucket_count()));
    }
}

TEST(integer_keys_with_an_identity_hash_still_spread_out) {
    // libstdc++ defines std::hash<int> as the identity function. With a
    // power-of-two bucket count, multiples of the count would all collide.
    // The prime bucket counts are what prevent that.
    IntTable table;
    for (int index = 0; index < 500; ++index) {
        table.insert_or_assign(index * 16, "v" + std::to_string(index));
    }

    EXPECT_EQ(table.size(), static_cast<std::size_t>(500));
    for (int index = 0; index < 500; ++index) {
        const std::string* value = table.find(index * 16);
        ASSERT_TRUE(value != nullptr);
        EXPECT_EQ(*value, "v" + std::to_string(index));
    }

    // No single bucket should hold anything close to all the keys.
    std::size_t longest_chain = 0;
    for (std::size_t bucket = 0; bucket < table.bucket_count(); ++bucket) {
        longest_chain = std::max(longest_chain, table.bucket_size(bucket));
    }
    EXPECT_TRUE(longest_chain < 10);
}

// -------------------------------------------------------------------------
// Clear
// -------------------------------------------------------------------------

TEST(clear_removes_every_entry_and_keeps_the_table_usable) {
    StringTable table;
    for (int index = 0; index < 100; ++index) {
        table.insert_or_assign("key" + std::to_string(index), index);
    }

    table.clear();

    EXPECT_TRUE(table.empty());
    EXPECT_EQ(table.size(), static_cast<std::size_t>(0));
    EXPECT_EQ(sorted_keys(table), std::string(""));
    EXPECT_FALSE(table.contains("key0"));
    EXPECT_TRUE(table.bucket_count() > 0);

    table.insert_or_assign("fresh", 1);
    EXPECT_EQ(table.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(*table.find("fresh"), 1);
}

TEST(clearing_a_very_long_chain_does_not_overflow_the_stack) {
    // Freeing a chain by destroying the head unique_ptr recurses once per
    // node. At 20000 nodes that is deep enough to exhaust a typical 1 MiB
    // stack, so this finishing at all is what demonstrates the iterative
    // teardown in clear().
    //
    // Building the chain is deliberately the quadratic worst case: every
    // insert walks the whole chain to check for a duplicate. That is the
    // O(n) side of "average O(1), worst case O(n)" being paid for real,
    // which is also why the count here is 20000 and not more.
    constexpr int kChainLength = 20000;

    CollidingTable table;
    for (int index = 0; index < kChainLength; ++index) {
        table.insert_or_assign("key" + std::to_string(index), index);
    }
    EXPECT_EQ(table.bucket_size(0), static_cast<std::size_t>(kChainLength));

    table.clear();
    EXPECT_TRUE(table.empty());
    EXPECT_EQ(table.bucket_size(0), static_cast<std::size_t>(0));
}

// -------------------------------------------------------------------------
// Value semantics
// -------------------------------------------------------------------------

TEST(a_copied_table_is_independent) {
    StringTable original;
    original.insert_or_assign("a", 1);
    original.insert_or_assign("b", 2);

    StringTable copy(original);
    EXPECT_EQ(copy.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(sorted_keys(copy), std::string("a,b"));

    copy.insert_or_assign("c", 3);
    copy.insert_or_assign("a", 100);

    // Changing the copy must not disturb the original -- proof that the
    // chains were duplicated rather than shared.
    EXPECT_EQ(original.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(*original.find("a"), 1);
    EXPECT_FALSE(original.contains("c"));
}

TEST(copy_assignment_replaces_the_target) {
    StringTable source;
    source.insert_or_assign("x", 1);

    StringTable target;
    target.insert_or_assign("old", 99);
    target = source;

    EXPECT_EQ(target.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(target.contains("x"));
    EXPECT_FALSE(target.contains("old"));
}

TEST(self_assignment_is_safe) {
    StringTable table;
    table.insert_or_assign("k", 1);

    const StringTable& alias = table;
    table = alias;

    EXPECT_EQ(table.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(*table.find("k"), 1);
}

TEST(a_moved_table_takes_the_entries) {
    StringTable source;
    source.insert_or_assign("a", 1);
    source.insert_or_assign("b", 2);

    StringTable moved(std::move(source));

    EXPECT_EQ(moved.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(sorted_keys(moved), std::string("a,b"));
}

TEST(a_moved_from_table_is_still_usable) {
    StringTable source;
    source.insert_or_assign("a", 1);

    const StringTable moved(std::move(source));

    // A moved-from object must be in a valid state. It has no buckets left,
    // so inserting has to give it some rather than divide by zero.
    EXPECT_EQ(source.size(), static_cast<std::size_t>(0));  // NOLINT(bugprone-use-after-move)
    EXPECT_FALSE(source.contains("a"));
    source.insert_or_assign("fresh", 5);
    EXPECT_EQ(source.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(*source.find("fresh"), 5);
}

TEST(move_assignment_transfers_the_entries) {
    StringTable source;
    source.insert_or_assign("a", 1);

    StringTable target;
    target.insert_or_assign("old", 99);
    target = std::move(source);

    EXPECT_EQ(target.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(target.contains("a"));
    EXPECT_FALSE(target.contains("old"));
}

// -------------------------------------------------------------------------
// Mixed and repeated operations
// -------------------------------------------------------------------------

TEST(repeated_insert_and_erase_cycles_keep_size_correct) {
    StringTable table;
    for (int round = 0; round < 1000; ++round) {
        EXPECT_TRUE(table.insert_or_assign("cycle", round));
        EXPECT_EQ(table.size(), static_cast<std::size_t>(1));
        EXPECT_TRUE(table.erase("cycle"));
        EXPECT_EQ(table.size(), static_cast<std::size_t>(0));
    }
    EXPECT_TRUE(table.empty());
}

TEST(interleaved_operations_stay_consistent) {
    StringTable table;
    constexpr int kCount = 1000;

    for (int index = 0; index < kCount; ++index) {
        table.insert_or_assign("key" + std::to_string(index), index);
    }
    for (int index = 0; index < kCount; index += 2) {
        EXPECT_TRUE(table.erase("key" + std::to_string(index)));
    }
    EXPECT_EQ(table.size(), static_cast<std::size_t>(kCount / 2));

    // Re-insert the erased half, into a table that has already grown.
    for (int index = 0; index < kCount; index += 2) {
        EXPECT_TRUE(table.insert_or_assign("key" + std::to_string(index), index * 10));
    }
    EXPECT_EQ(table.size(), static_cast<std::size_t>(kCount));

    for (int index = 0; index < kCount; ++index) {
        const int* value = table.find("key" + std::to_string(index));
        ASSERT_TRUE(value != nullptr);
        EXPECT_EQ(*value, index % 2 == 0 ? index * 10 : index);
    }
}

TEST(erasing_everything_empties_the_table_completely) {
    StringTable table;
    constexpr int kCount = 500;
    for (int index = 0; index < kCount; ++index) {
        table.insert_or_assign("key" + std::to_string(index), index);
    }
    for (int index = 0; index < kCount; ++index) {
        EXPECT_TRUE(table.erase("key" + std::to_string(index)));
    }

    EXPECT_TRUE(table.empty());
    EXPECT_EQ(sorted_keys(table), std::string(""));

    std::size_t remaining = 0;
    for (std::size_t bucket = 0; bucket < table.bucket_count(); ++bucket) {
        remaining += table.bucket_size(bucket);
    }
    EXPECT_EQ(remaining, static_cast<std::size_t>(0));
}

TEST(string_values_are_stored_and_moved_correctly) {
    HashTable<std::string, std::string> table;
    table.insert_or_assign("greeting", "hello world");
    table.insert_or_assign("empty", "");

    ASSERT_TRUE(table.find("greeting") != nullptr);
    EXPECT_EQ(*table.find("greeting"), std::string("hello world"));
    EXPECT_EQ(*table.find("empty"), std::string(""));
    EXPECT_TRUE(table.contains("empty"));
}

TEST(for_each_visits_every_entry_exactly_once) {
    StringTable table;
    constexpr int kCount = 300;
    for (int index = 0; index < kCount; ++index) {
        table.insert_or_assign("key" + std::to_string(index), index);
    }

    std::size_t visited = 0;
    long long sum = 0;
    table.for_each([&visited, &sum](const std::string&, int value) {
        ++visited;
        sum += value;
    });

    EXPECT_EQ(visited, static_cast<std::size_t>(kCount));
    // 0 + 1 + ... + 299; a duplicate or skipped visit would change this.
    EXPECT_EQ(sum, static_cast<long long>(kCount) * (kCount - 1) / 2);
}

// -------------------------------------------------------------------------
// Destruction -- every value created is destroyed exactly once
// -------------------------------------------------------------------------

TEST(the_table_destroys_every_value_it_stores) {
    Counted::live = 0;
    {
        HashTable<std::string, Counted> table;
        for (int index = 0; index < 500; ++index) {
            table.insert_or_assign("key" + std::to_string(index), Counted(index));
        }
        // One live value per entry. More would mean rehashing duplicated
        // entries; fewer would mean it dropped them.
        EXPECT_EQ(Counted::live, 500);

        for (int index = 0; index < 500; index += 2) {
            EXPECT_TRUE(table.erase("key" + std::to_string(index)));
        }
        EXPECT_EQ(Counted::live, 250);

        table.clear();
        EXPECT_EQ(Counted::live, 0);
    }
    EXPECT_EQ(Counted::live, 0);
}

TEST(overwriting_a_value_does_not_accumulate_instances) {
    Counted::live = 0;
    {
        HashTable<std::string, Counted> table;
        for (int round = 0; round < 100; ++round) {
            table.insert_or_assign("same", Counted(round));
        }
        EXPECT_EQ(table.size(), static_cast<std::size_t>(1));
        EXPECT_EQ(Counted::live, 1);
    }
    EXPECT_EQ(Counted::live, 0);
}

TEST(destroying_a_table_without_clearing_frees_everything) {
    // The destructor is the path a normal caller takes; clear() is not
    // required for the table to release its entries.
    Counted::live = 0;
    {
        HashTable<std::string, Counted> table;
        for (int index = 0; index < 300; ++index) {
            table.insert_or_assign("key" + std::to_string(index), Counted(index));
        }
        EXPECT_EQ(Counted::live, 300);
    }
    EXPECT_EQ(Counted::live, 0);
}

TEST(copying_a_table_duplicates_the_values_and_frees_both) {
    Counted::live = 0;
    {
        HashTable<std::string, Counted> original;
        for (int index = 0; index < 50; ++index) {
            original.insert_or_assign("key" + std::to_string(index), Counted(index));
        }
        EXPECT_EQ(Counted::live, 50);

        {
            const HashTable<std::string, Counted> copy(original);
            // A copy that shared nodes would leave this at 50.
            EXPECT_EQ(Counted::live, 100);
            EXPECT_EQ(copy.size(), static_cast<std::size_t>(50));
        }
        EXPECT_EQ(Counted::live, 50);
    }
    EXPECT_EQ(Counted::live, 0);
}

TEST(moving_a_table_does_not_duplicate_the_values) {
    Counted::live = 0;
    {
        HashTable<std::string, Counted> source;
        for (int index = 0; index < 50; ++index) {
            source.insert_or_assign("key" + std::to_string(index), Counted(index));
        }
        EXPECT_EQ(Counted::live, 50);

        const HashTable<std::string, Counted> moved(std::move(source));
        // Moving transfers the buckets; the values themselves do not move.
        EXPECT_EQ(Counted::live, 50);
        EXPECT_EQ(moved.size(), static_cast<std::size_t>(50));
    }
    EXPECT_EQ(Counted::live, 0);
}

MINIDB_TEST_MAIN()
