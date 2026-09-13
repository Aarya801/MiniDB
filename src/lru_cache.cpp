#include "minidb/lru_cache.hpp"

#include <utility>

namespace minidb {

LruCache::LruCache(std::size_t capacity) : capacity_(capacity) {}

LruCache::LruCache(const LruCache& other) : LruCache(other.capacity_) {
    // Rebuild iterators into our own list, preserving the original recency.
    for (auto it = other.entries_.rbegin(); it != other.entries_.rend(); ++it) {
        put(it->first, it->second);
    }
}

LruCache::LruCache(LruCache&& other) noexcept : LruCache(0) {
    swap(other);
}

LruCache& LruCache::operator=(LruCache other) noexcept {
    swap(other);
    return *this;
}

void LruCache::swap(LruCache& other) noexcept {
    std::swap(capacity_, other.capacity_);
    entries_.swap(other.entries_);
    index_.swap(other.index_);
}

const Value* LruCache::get(std::string_view key) {
    const auto found = index_.find(key);
    if (found == index_.end()) {
        return nullptr;
    }
    entries_.splice(entries_.begin(), entries_, found->second);
    return &found->second->second;
}

void LruCache::put(Key key, Value value) {
    if (capacity_ == 0) {
        return;
    }
    const auto found = index_.find(key);
    if (found != index_.end()) {
        found->second->second.swap(value);
        entries_.splice(entries_.begin(), entries_, found->second);
        return;
    }

    // Allocate both structures before evicting anything. If map insertion
    // throws, roll back the new list node and preserve all prior entries.
    entries_.emplace_front(std::move(key), std::move(value));
    try {
        index_.emplace(entries_.front().first, entries_.begin());
    } catch (...) {
        entries_.pop_front();
        throw;
    }
    if (entries_.size() > capacity_) {
        index_.erase(entries_.back().first);
        entries_.pop_back();
    }
}

bool LruCache::contains(std::string_view key) const {
    return index_.find(key) != index_.end();
}

bool LruCache::erase(std::string_view key) {
    const auto found = index_.find(key);
    if (found == index_.end()) {
        return false;
    }
    entries_.erase(found->second);
    index_.erase(found);
    return true;
}

void LruCache::clear() noexcept {
    index_.clear();
    entries_.clear();
}

}  // namespace minidb
