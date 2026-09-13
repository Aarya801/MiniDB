#ifndef MINIDB_TRANSACTION_HPP
#define MINIDB_TRANSACTION_HPP

#include <cstddef>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "minidb/result.hpp"
#include "minidb/types.hpp"

namespace minidb {

class Database;

enum class TransactionOperation {
    Set,
    Delete,
};

struct TransactionMutation {
    TransactionOperation operation;
    Key key;
    Value value;
};

/// A single active/inactive transaction bound to one Database.
/// Changes remain in this overlay until commit; rollback only drops the overlay.
class Transaction {
public:
    explicit Transaction(Database& database) noexcept : database_(database) {}
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] std::size_t change_count() const noexcept { return changes_.size(); }

    Result begin();
    Result set(std::string_view key, std::string_view value);
    [[nodiscard]] Result get(std::string_view key) const;
    Result remove(std::string_view key);
    [[nodiscard]] bool exists(std::string_view key) const;
    [[nodiscard]] std::vector<Key> keys() const;
    Result clear();
    Result commit();
    Result rollback();

private:
    struct StringHash {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view key) const noexcept;
    };

    using Changes = std::unordered_map<Key, std::optional<Value>, StringHash, std::equal_to<>>;

    Database& database_;
    Changes changes_;
    bool active_ = false;
};

}  // namespace minidb

#endif  // MINIDB_TRANSACTION_HPP
