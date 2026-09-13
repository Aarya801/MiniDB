#include "minidb/database.hpp"

#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

Database::Database(std::filesystem::path snapshot_path, std::size_t cache_capacity)
    : cache_(cache_capacity) {
    // The log lives beside the snapshot under a derived name, so one path
    // from the caller configures both files.
    std::filesystem::path log_path = snapshot_path;
    log_path += ".wal";

    storage_.emplace(std::move(snapshot_path));
    wal_.emplace(std::move(log_path));
}

void Database::apply_recovered(EntryTable& table, const WalRecord& record) {
    switch (record.operation) {
        case WalOperation::Set:
            table.insert_or_assign(record.key, record.value);
            break;
        case WalOperation::Delete:
            // A delete of a key the snapshot never had is not an error during
            // recovery: the log may be replayed over a snapshot that already
            // reflects it.
            table.erase(record.key);
            break;
        case WalOperation::Clear:
            table.clear();
            break;
    }
}

const std::filesystem::path& Database::snapshot_path() const {
    if (!storage_.has_value()) {
        throw std::logic_error("Database::snapshot_path called on an in-memory database");
    }
    return storage_->path();
}

bool Database::snapshot_exists() const {
    return storage_.has_value() && storage_->snapshot_exists();
}

const std::filesystem::path& Database::wal_path() const {
    if (!wal_.has_value()) {
        throw std::logic_error("Database::wal_path called on an in-memory database");
    }
    return wal_->path();
}

Result Database::load() {
    if (!storage_.has_value()) {
        return Result::failure(StatusCode::InvalidArgument,
                               "this database is in memory only and has nothing to load");
    }
    loaded_ = false;
    replayed_operation_count_ = 0;

    // Step 1: the snapshot, which is the state as of the last successful save.
    std::vector<Record> records;
    std::uint64_t checkpoint = 0;
    Result read = storage_->load(records, &checkpoint);
    if (!read.is_ok() && read.code() != StatusCode::NotFound) {
        // Corrupt, unsupported or unreadable. Leave the database untouched
        // and let the caller decide; silently continuing with no data would
        // look exactly like a successful load of an empty database.
        return read;
    }

    // Recovery builds a separate table and swaps it in only once every step
    // has succeeded, so a failure partway through cannot leave the database
    // holding half a snapshot or half a log.
    EntryTable recovered(records.size() + 1);
    for (Record& record : records) {
        recovered.insert_or_assign(record.key, std::move(record.value));
    }

    // Step 2: the log, which holds everything done since that snapshot.
    std::vector<WalRecord> logged;
    Result replayed = wal_->replay(logged, checkpoint);
    if (!replayed.is_ok() && replayed.code() != StatusCode::NotFound) {
        // A damaged log is not an empty log. Refusing here keeps the files on
        // disk exactly as they are, so nothing recoverable is destroyed.
        return replayed;
    }
    if (replayed.is_ok()) {
        for (const WalRecord& record : logged) {
            apply_recovered(recovered, record);
        }
        replayed_operation_count_ = logged.size();
    }

    entries_ = std::move(recovered);
    cache_.clear();
    loaded_ = true;
    return Result::ok();
}

Result Database::ensure_loaded() {
    if (storage_.has_value() && !loaded_) {
        return load();
    }
    return Result::ok();
}

Result Database::save() {
    if (!storage_.has_value()) {
        return Result::failure(StatusCode::InvalidArgument,
                               "this database is in memory only and cannot be saved");
    }

    Result ready = ensure_loaded();
    if (!ready.is_ok()) {
        return ready;
    }

    std::vector<Record> records;
    records.reserve(entries_.size());
    entries_.for_each(
        [&records](const Key& key, const Value& value) { records.push_back(Record{key, value}); });

    const std::uint64_t checkpoint = wal_->last_sequence();
    Result saved = storage_->save(records, checkpoint);
    if (!saved.is_ok()) {
        // The snapshot did not happen, so the log is still the only record of
        // these mutations. Resetting it here would destroy them.
        return saved;
    }

    // Only now, with the snapshot safely in place, is the log redundant.
    Result reset = wal_->reset(checkpoint);
    if (!reset.is_ok()) {
        loaded_ = false;
    }
    return reset;
}

Result Database::set(std::string_view key, std::string_view value) {
    Result key_check = validate_key(key);
    if (!key_check.is_ok()) {
        return key_check;
    }
    if (value.size() > limits::kMaxValueSize) {
        return Result::failure(StatusCode::ValueTooLarge,
                               "value exceeds " + std::to_string(limits::kMaxValueSize) + " bytes");
    }

    Result ready = ensure_loaded();
    if (!ready.is_ok()) {
        return ready;
    }

    // Write-ahead ordering: the log record is appended and flushed before
    // memory changes. If the append fails the mutation does not happen at
    // all, so the log can never be missing something the database has.
    if (wal_.has_value()) {
        Result logged = wal_->append_set(key, value);
        if (!logged.is_ok()) {
            loaded_ = false;
            return logged;
        }
    }

    cache_.erase(key);
    // insert_or_assign constructs a Key only when the entry is new, so
    // overwriting an existing key allocates nothing for the key itself.
    try {
        entries_.insert_or_assign(key, Value(value));
    } catch (...) {
        // The WAL may contain a SET which memory could not allocate. Force
        // recovery before another mutation or a snapshot can discard it.
        loaded_ = false;
        throw;
    }
    return Result::ok();
}

Result Database::get(std::string_view key) const {
    Result key_check = validate_key(key);
    if (!key_check.is_ok()) {
        return key_check;
    }

    if (const Value* cached = cache_.get(key)) {
        return Result::ok(*cached);
    }
    const Value* value = entries_.find(key);
    if (value == nullptr) {
        // Deliberately no message: the CLI prints this verbatim as NOT_FOUND.
        return Result::failure(StatusCode::NotFound);
    }
    if (cache_.capacity() != 0) {
        try {
            cache_.put(Key(key), *value);
        } catch (const std::bad_alloc&) {
            // Optional caching must not fail an otherwise successful read.
        }
    }
    return Result::ok(*value);
}

Result Database::remove(std::string_view key) {
    Result key_check = validate_key(key);
    if (!key_check.is_ok()) {
        return key_check;
    }

    Result ready = ensure_loaded();
    if (!ready.is_ok()) {
        return ready;
    }

    // Checked before logging: a delete of a key that is not there changes
    // nothing, and logging it would put a record in the log for an operation
    // that never happened.
    if (!entries_.contains(key)) {
        return Result::failure(StatusCode::NotFound);
    }

    if (wal_.has_value()) {
        Result logged = wal_->append_delete(key);
        if (!logged.is_ok()) {
            loaded_ = false;
            return logged;
        }
    }

    cache_.erase(key);
    entries_.erase(key);
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

Result Database::clear() {
    Result ready = ensure_loaded();
    if (!ready.is_ok()) {
        return ready;
    }
    // Logged as one record rather than as a delete per key: a half-applied
    // clear would leave the log describing a state that never existed.
    if (wal_.has_value()) {
        Result logged = wal_->append_clear();
        if (!logged.is_ok()) {
            loaded_ = false;
            return logged;
        }
    }

    entries_.clear();
    cache_.clear();
    return Result::ok();
}

std::size_t Database::size() const noexcept {
    return entries_.size();
}

bool Database::empty() const noexcept {
    return entries_.empty();
}

}  // namespace minidb
