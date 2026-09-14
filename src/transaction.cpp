#include "minidb/transaction.hpp"

#include <algorithm>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

#include "minidb/database.hpp"

namespace minidb {
namespace {

[[nodiscard]] Result require_key(std::string_view key) {
    if (key.empty()) {
        return Result::failure(StatusCode::InvalidArgument, "key must not be empty");
    }
    if (key.size() > limits::kMaxKeySize) {
        return Result::failure(StatusCode::KeyTooLarge,
                               "key exceeds " + std::to_string(limits::kMaxKeySize) + " bytes");
    }
    return Result::ok();
}

[[nodiscard]] Result inactive_error(std::string_view operation) {
    return Result::failure(StatusCode::InvalidTransactionState,
                           std::string(operation) + " requires an active transaction");
}

}  // namespace

std::size_t Transaction::StringHash::operator()(std::string_view key) const noexcept {
    return std::hash<std::string_view>{}(key);
}

bool Transaction::active() const {
    std::lock_guard lock(mutex_);
    return active_;
}

std::size_t Transaction::change_count() const {
    std::lock_guard lock(mutex_);
    return changes_.size();
}

Result Transaction::begin() {
    std::lock_guard lock(mutex_);
    if (active_) {
        return Result::failure(StatusCode::InvalidTransactionState,
                               "a transaction is already active");
    }
    Result ready = database_.ensure_loaded();
    if (!ready.is_ok()) {
        return ready;
    }
    changes_.clear();
    active_ = true;
    return Result::ok();
}

Result Transaction::set(std::string_view key, std::string_view value) {
    std::lock_guard lock(mutex_);
    if (!active_) {
        return inactive_error("SET");
    }
    Result key_check = require_key(key);
    if (!key_check.is_ok()) {
        return key_check;
    }
    if (value.size() > limits::kMaxValueSize) {
        return Result::failure(StatusCode::ValueTooLarge,
                               "value exceeds " + std::to_string(limits::kMaxValueSize) + " bytes");
    }
    changes_.insert_or_assign(Key(key), Value(value));
    return Result::ok();
}

Result Transaction::get(std::string_view key) const {
    std::lock_guard lock(mutex_);
    return get_unlocked(key);
}

Result Transaction::get_unlocked(std::string_view key) const {
    if (!active_) {
        return inactive_error("GET");
    }
    Result key_check = require_key(key);
    if (!key_check.is_ok()) {
        return key_check;
    }
    const auto changed = changes_.find(key);
    if (changed != changes_.end()) {
        return changed->second.has_value() ? Result::ok(*changed->second)
                                           : Result::failure(StatusCode::NotFound);
    }
    return database_.get(key);
}

Result Transaction::remove(std::string_view key) {
    std::lock_guard lock(mutex_);
    if (!active_) {
        return inactive_error("DELETE");
    }
    Result current = get_unlocked(key);
    if (!current.is_ok()) {
        return current;
    }
    changes_.insert_or_assign(Key(key), std::nullopt);
    return Result::ok();
}

bool Transaction::exists(std::string_view key) const {
    std::lock_guard lock(mutex_);
    return active_ && get_unlocked(key).is_ok();
}

std::vector<Key> Transaction::keys() const {
    std::lock_guard lock(mutex_);
    return keys_unlocked();
}

std::vector<Key> Transaction::keys_unlocked() const {
    if (!active_) {
        return {};
    }
    std::unordered_set<Key, StringHash, std::equal_to<>> visible;
    for (Key& key : database_.keys()) {
        visible.insert(std::move(key));
    }
    for (const auto& [key, value] : changes_) {
        if (value.has_value()) {
            visible.insert(key);
        } else {
            visible.erase(key);
        }
    }
    return {visible.begin(), visible.end()};
}

Result Transaction::clear() {
    std::lock_guard lock(mutex_);
    if (!active_) {
        return inactive_error("CLEAR");
    }
    for (Key& key : keys_unlocked()) {
        changes_.insert_or_assign(std::move(key), std::nullopt);
    }
    return Result::ok();
}

Result Transaction::commit() {
    std::lock_guard lock(mutex_);
    if (!active_) {
        return inactive_error("COMMIT");
    }
    std::vector<TransactionMutation> mutations;
    mutations.reserve(changes_.size());
    for (const auto& [key, value] : changes_) {
        mutations.push_back(TransactionMutation{
            value.has_value() ? TransactionOperation::Set : TransactionOperation::Delete, key,
            value.value_or(Value{})});
    }
    std::sort(mutations.begin(), mutations.end(),
              [](const TransactionMutation& left, const TransactionMutation& right) {
                  return left.key < right.key;
              });

    Result committed = database_.commit_transaction(mutations);
    changes_.clear();
    active_ = false;
    return committed;
}

Result Transaction::rollback() {
    std::lock_guard lock(mutex_);
    if (!active_) {
        return inactive_error("ROLLBACK");
    }
    changes_.clear();
    active_ = false;
    return Result::ok();
}

}  // namespace minidb
