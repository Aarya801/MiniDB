#ifndef MINIDB_HASH_TABLE_HPP
#define MINIDB_HASH_TABLE_HPP

#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace minidb {

namespace detail {

/// Returns a bucket count of at least `minimum`, drawn from a table of primes
/// that roughly double. Defined in hash_table.cpp because it does not depend
/// on the template parameters.
///
/// Why primes: the bucket index is `hash % bucket_count`, so the modulus
/// decides which bits of the hash survive. A power-of-two modulus keeps only
/// the low bits, and libstdc++ defines std::hash<int> as the identity
/// function -- with 16 buckets the keys 0, 16, 32, 48 would all land in
/// bucket 0. A prime modulus mixes every bit of the hash into the result, so
/// a weak hash degrades gracefully instead of collapsing.
[[nodiscard]] std::size_t next_bucket_count(std::size_t minimum);

}  // namespace detail

/// A hash table with separate chaining.
///
/// This replaces std::unordered_map inside Database from Milestone 2 onward.
/// It is a normal container: it owns its entries, copies and moves by value,
/// and frees everything in its destructor.
///
/// How a key becomes a bucket:
///
///     key -> Hash{}(key) -> std::size_t hash -> hash % bucket_count -> index
///
/// Every key that lands on the same index is kept in a singly linked chain
/// hanging off that bucket. Lookup walks the chain comparing keys, so a
/// collision costs extra comparisons but never loses data.
///
/// Complexity, where n is the number of entries:
///
///   insert_or_assign  average O(1), worst case O(n)
///   find / contains   average O(1), worst case O(n)
///   erase             average O(1), worst case O(n)
///   for_each          O(n + bucket_count)
///   clear             O(n)
///   rehash            O(n)
///   space             O(n + bucket_count)
///
/// The averages hold only while two conditions do: the hash spreads keys
/// evenly, and the load factor stays bounded so chains stay short. The worst
/// case is every key colliding into one bucket, which turns the table into a
/// linked list. Neither condition is an accident -- the load factor is
/// enforced by rehashing, and the hash is the caller's choice.
///
/// Reference stability: rehashing relinks existing nodes rather than
/// recreating them, so a pointer returned by find() stays valid across a
/// rehash. It is invalidated only by erasing that entry, or by clear().
template<typename Key, typename Value, typename Hash = std::hash<Key>>
class HashTable {
public:
    /// Starting bucket count. Small enough not to waste memory on a table
    /// that stays tiny, large enough that the first few inserts do not each
    /// trigger a rehash.
    static constexpr std::size_t kDefaultBucketCount = 17;

    /// Grow once the table is three-quarters full.
    ///
    /// The threshold trades memory against collisions. At 1.0 chains average
    /// one node but long chains are already common; at 0.5 lookups are fast
    /// but half the bucket array is idle. 0.75 keeps the expected chain
    /// length below one while leaving a quarter of the array spare, and is
    /// the same value the JDK settled on for HashMap.
    static constexpr float kMaxLoadFactor = 0.75F;

    HashTable() : HashTable(kDefaultBucketCount) {}

    explicit HashTable(std::size_t initial_bucket_count)
        : buckets_(detail::next_bucket_count(initial_bucket_count)) {}

    // Rule of five. The destructor has to be written by hand (see clear()),
    // and declaring it suppresses the implicit move operations, so every
    // member of the set is spelled out.

    ~HashTable() { clear(); }

    HashTable(const HashTable& other)
        : buckets_(other.buckets_.size()), size_(0), hasher_(other.hasher_) {
        // Copy chains node by node. Copying the unique_ptrs is impossible by
        // design, which is exactly what stops two tables sharing nodes.
        other.for_each(
            [this](const Key& key, const Value& value) { insert_or_assign(key, value); });
    }

    HashTable& operator=(const HashTable& other) {
        // Copy and swap: if the copy throws, *this is untouched.
        if (this != &other) {
            HashTable copy(other);
            swap(copy);
        }
        return *this;
    }

    HashTable(HashTable&& other) noexcept(std::is_nothrow_move_constructible_v<Hash>)
        : buckets_(std::move(other.buckets_)),
          size_(other.size_),
          hasher_(std::move(other.hasher_)) {
        other.size_ = 0;
    }

    /// Move assignment by swap. The old contents travel into `other` and are
    /// destroyed by its destructor, which frees chains iteratively.
    HashTable& operator=(HashTable&& other) noexcept(std::is_nothrow_swappable_v<Hash>) {
        swap(other);
        return *this;
    }

    void swap(HashTable& other) noexcept(std::is_nothrow_swappable_v<Hash>) {
        buckets_.swap(other.buckets_);
        std::swap(size_, other.size_);
        using std::swap;
        swap(hasher_, other.hasher_);
    }

    /// Stores `value` under `key`, replacing any existing entry.
    /// Returns true if a new entry was created, false if one was replaced.
    ///
    /// `KeyLike` may be any type the hasher accepts and that compares equal
    /// to a Key -- a std::string_view against std::string keys, for example.
    /// A Key is constructed only when a new node is inserted, so overwriting
    /// an existing entry allocates nothing for the key.
    template<typename KeyLike>
    bool insert_or_assign(const KeyLike& key, Value value) {
        // A moved-from table has no buckets; give it some before indexing.
        if (buckets_.empty()) {
            buckets_.resize(kDefaultBucketCount);
        }

        const std::size_t hash = hash_of(key);
        Node* existing = find_node(hash, key);
        if (existing != nullptr) {
            existing->value = std::move(value);
            return false;
        }

        if (would_exceed_load_factor()) {
            grow();
        }

        // Push onto the front of the chain: O(1), and no need to walk to the
        // end. Chain order is not part of the container's contract.
        const std::size_t index = hash % buckets_.size();
        auto node = std::make_unique<Node>(Key(key), std::move(value), hash);
        node->next = std::move(buckets_[index]);
        buckets_[index] = std::move(node);
        ++size_;
        return true;
    }

    /// Returns a pointer to the stored value, or nullptr if the key is absent.
    /// A pointer rather than a reference so that "missing" has a
    /// representation the caller must handle.
    template<typename KeyLike>
    [[nodiscard]] Value* find(const KeyLike& key) {
        Node* node = find_node(hash_of(key), key);
        return node == nullptr ? nullptr : &node->value;
    }

    template<typename KeyLike>
    [[nodiscard]] const Value* find(const KeyLike& key) const {
        const Node* node = find_node(hash_of(key), key);
        return node == nullptr ? nullptr : &node->value;
    }

    template<typename KeyLike>
    [[nodiscard]] bool contains(const KeyLike& key) const {
        return find_node(hash_of(key), key) != nullptr;
    }

    /// Removes `key`. Returns true if something was removed.
    template<typename KeyLike>
    bool erase(const KeyLike& key) {
        if (buckets_.empty()) {
            return false;
        }

        const std::size_t hash = hash_of(key);
        // `link` points at whatever owns the node under inspection: the
        // bucket slot for the first node, the previous node's `next`
        // afterwards. That removes the need for a special case at the head.
        std::unique_ptr<Node>* link = &buckets_[hash % buckets_.size()];
        while (*link != nullptr) {
            Node* node = link->get();
            if (node->hash == hash && node->key == key) {
                std::unique_ptr<Node> removed = std::move(*link);
                *link = std::move(removed->next);
                --size_;
                return true;  // `removed` frees the node here, and only it
            }
            link = &node->next;
        }
        return false;
    }

    /// Removes every entry, keeping the bucket array.
    ///
    /// The loop is not decoration. Destroying a bucket would destroy its
    /// unique_ptr, which destroys the next node, and so on -- recursion as
    /// deep as the chain is long, which overflows the stack on the very case
    /// this container is built to survive: thousands of colliding keys.
    /// Unlinking each node before releasing it keeps the depth at one.
    void clear() noexcept {
        for (std::unique_ptr<Node>& bucket : buckets_) {
            std::unique_ptr<Node> node = std::move(bucket);
            while (node != nullptr) {
                // Detach the tail first, so releasing `node` frees one node.
                std::unique_ptr<Node> next = std::move(node->next);
                node = std::move(next);
            }
        }
        size_ = 0;
    }

    /// Calls `visit(key, value)` for every entry, in unspecified order.
    ///
    /// A full iterator type would need to skip empty buckets in both
    /// directions and satisfy the iterator requirements -- a lot of
    /// boilerplate for a container whose only traversal is KEYS. This is the
    /// traversal that is actually needed.
    template<typename Visitor>
    void for_each(Visitor&& visit) const {
        for (const std::unique_ptr<Node>& bucket : buckets_) {
            for (const Node* node = bucket.get(); node != nullptr; node = node->next.get()) {
                visit(node->key, node->value);
            }
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t bucket_count() const noexcept { return buckets_.size(); }

    [[nodiscard]] float load_factor() const noexcept {
        if (buckets_.empty()) {
            return 0.0F;
        }
        return static_cast<float>(size_) / static_cast<float>(buckets_.size());
    }

    [[nodiscard]] static constexpr float max_load_factor() noexcept { return kMaxLoadFactor; }

    /// The bucket a key maps to. Exposed so tests can demonstrate collisions
    /// directly rather than inferring them.
    template<typename KeyLike>
    [[nodiscard]] std::size_t bucket_index(const KeyLike& key) const noexcept {
        return buckets_.empty() ? 0 : hash_of(key) % buckets_.size();
    }

    /// Number of entries chained in one bucket. Lets tests assert that
    /// separate chaining actually holds several entries at one index.
    [[nodiscard]] std::size_t bucket_size(std::size_t index) const noexcept {
        if (index >= buckets_.size()) {
            return 0;
        }
        std::size_t count = 0;
        for (const Node* node = buckets_[index].get(); node != nullptr; node = node->next.get()) {
            ++count;
        }
        return count;
    }

    /// Resizes to at least `minimum_buckets` and redistributes every entry.
    ///
    /// Never shrinks below what the current entries need: a request for fewer
    /// buckets than size() / kMaxLoadFactor is raised to that floor, so no
    /// caller can push the load factor past the threshold the rest of the
    /// class relies on. std::unordered_map::rehash makes the same guarantee.
    void rehash(std::size_t minimum_buckets) {
        const auto required =
            static_cast<std::size_t>(static_cast<float>(size_) / kMaxLoadFactor) + 1;
        const std::size_t target = minimum_buckets > required ? minimum_buckets : required;
        const std::size_t new_count = detail::next_bucket_count(target);
        if (new_count == buckets_.size()) {
            return;
        }

        // Built first: if this allocation throws, the table is untouched.
        std::vector<std::unique_ptr<Node>> new_buckets(new_count);

        for (std::unique_ptr<Node>& bucket : buckets_) {
            std::unique_ptr<Node> node = std::move(bucket);
            while (node != nullptr) {
                std::unique_ptr<Node> next = std::move(node->next);

                // The hash was cached at insertion, so redistributing costs
                // one modulo per entry and never re-hashes a key.
                const std::size_t index = node->hash % new_count;
                node->next = std::move(new_buckets[index]);
                new_buckets[index] = std::move(node);

                node = std::move(next);
            }
        }
        buckets_.swap(new_buckets);
    }

private:
    struct Node {
        Node(Key node_key, Value node_value, std::size_t node_hash)
            : key(std::move(node_key)), value(std::move(node_value)), hash(node_hash) {}

        Key key;
        Value value;

        /// The key's hash, cached at insertion. It makes rehashing cheap and
        /// lets a chain walk reject a non-matching node with an integer
        /// comparison before touching the key itself.
        std::size_t hash;

        std::unique_ptr<Node> next;
    };

    template<typename KeyLike>
    [[nodiscard]] std::size_t hash_of(const KeyLike& key) const noexcept {
        return hasher_(key);
    }

    template<typename KeyLike>
    [[nodiscard]] Node* find_node(std::size_t hash, const KeyLike& key) {
        if (buckets_.empty()) {
            return nullptr;
        }
        for (Node* node = buckets_[hash % buckets_.size()].get(); node != nullptr;
             node = node->next.get()) {
            if (node->hash == hash && node->key == key) {
                return node;
            }
        }
        return nullptr;
    }

    template<typename KeyLike>
    [[nodiscard]] const Node* find_node(std::size_t hash, const KeyLike& key) const {
        if (buckets_.empty()) {
            return nullptr;
        }
        for (const Node* node = buckets_[hash % buckets_.size()].get(); node != nullptr;
             node = node->next.get()) {
            if (node->hash == hash && node->key == key) {
                return node;
            }
        }
        return nullptr;
    }

    /// True when adding one more entry would push the load factor past the
    /// threshold. Checked before inserting, so the table grows just in time.
    [[nodiscard]] bool would_exceed_load_factor() const noexcept {
        return static_cast<float>(size_ + 1) / static_cast<float>(buckets_.size()) > kMaxLoadFactor;
    }

    void grow() { rehash(buckets_.size() * 2); }

    std::vector<std::unique_ptr<Node>> buckets_;
    std::size_t size_ = 0;
    Hash hasher_{};
};

template<typename Key, typename Value, typename Hash>
void swap(HashTable<Key, Value, Hash>& left,
          HashTable<Key, Value, Hash>& right) noexcept(std::is_nothrow_swappable_v<Hash>) {
    left.swap(right);
}

}  // namespace minidb

#endif  // MINIDB_HASH_TABLE_HPP
