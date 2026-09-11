#include "minidb/database.hpp"

#include <string>

namespace minidb {
namespace {

/// Checks a key against the rules every entry point shares, so that set, get
/// and remove cannot drift apart in what they accept.
Result validate_key(std::string_view key) {
    if (key.empty()) {
        return Result::failure(StatusCode::InvalidArgument, "key must not be empty");
    }
    if (key.size() > limits::kMaxKeySize) {
        return Result::failure(StatusCode::KeyTooLarge,
                               "key exceeds " + std::to_string(limits::kMaxKeySize) + " bytes");
    }
    return Result::ok();
}

}  // namespace

Result Database::set(std::string_view key, std::string_view value) {
    Result key_check = validate_key(key);
    if (!key_check.is_ok()) {
        return key_check;
    }
    if (value.size() > limits::kMaxValueSize) {
        return Result::failure(StatusCode::ValueTooLarge,
                               "value exceeds " + std::to_string(limits::kMaxValueSize) + " bytes");
    }

    // insert_or_assign constructs a Key only when the entry is new, so
    // overwriting an existing key allocates nothing for the key itself.
    entries_.insert_or_assign(key, Value(value));
    return Result::ok();
}

Result Database::get(std::string_view key) const {
    Result key_check = validate_key(key);
    if (!key_check.is_ok()) {
        return key_check;
    }

    const Value* value = entries_.find(key);
    if (value == nullptr) {
        // Deliberately no message: the CLI prints this verbatim as NOT_FOUND.
        return Result::failure(StatusCode::NotFound);
    }
    return Result::ok(*value);
}

Result Database::remove(std::string_view key) {
    Result key_check = validate_key(key);
    if (!key_check.is_ok()) {
        return key_check;
    }

    if (!entries_.erase(key)) {
        return Result::failure(StatusCode::NotFound);
    }
    return Result::ok();
}

bool Database::exists(std::string_view key) const {
    if (!validate_key(key).is_ok()) {
        return false;
    }
    return entries_.contains(key);
}

std::vector<Key> Database::keys() const {
    std::vector<Key> result;
    result.reserve(entries_.size());
    entries_.for_each([&result](const Key& key, const Value&) { result.push_back(key); });
    return result;
}

void Database::clear() noexcept {
    entries_.clear();
}

std::size_t Database::size() const noexcept {
    return entries_.size();
}

bool Database::empty() const noexcept {
    return entries_.empty();
}

}  // namespace minidb
