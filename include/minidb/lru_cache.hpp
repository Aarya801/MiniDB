#ifndef MINIDB_LRU_CACHE_HPP
#define MINIDB_LRU_CACHE_HPP

#include <cstddef>
#include <functional>
#include <list>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "minidb/types.hpp"

namespace minidb {

/// Entry-bounded LRU: front is MRU, back is LRU. Zero capacity disables storage.
/// Hash lookup is average O(1); list promotion/unlink is O(1). String hashing
/// and copying still cost time proportional to the bytes involved.
class LruCache {
public:
    explicit LruCache(std::size_t capacity);
    LruCache(const LruCache& other);
    LruCache(LruCache&& other) noexcept;
    LruCache& operator=(LruCache other) noexcept;
    ~LruCache() = default;

    void swap(LruCache& other) noexcept;

    /// Hit promotes to MRU. Pointer is non-owning and is invalidated when that
    /// entry is erased/evicted, or the cache is cleared/assigned/destroyed.
    [[nodiscard]] const Value* get(std::string_view key);
    void put(Key key, Value value);
    /// Membership inspection does not change recency.
    [[nodiscard]] bool contains(std::string_view key) const;
    bool erase(std::string_view key);
    void clear() noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    using Entries = std::list<std::pair<Key, Value>>;
    struct StringHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view key) const noexcept {
            return std::hash<std::string_view>{}(key);
        }
    };
    std::size_t capacity_;
    Entries entries_;
    std::unordered_map<Key, Entries::iterator, StringHash, std::equal_to<>> index_;
};

}  // namespace minidb
#endif  // MINIDB_LRU_CACHE_HPP
