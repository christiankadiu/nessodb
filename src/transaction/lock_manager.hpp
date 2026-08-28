#pragma once

#include "common/table_id.hpp"
#include "common/transaction_id.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace minidb::transaction {

class TransactionManager;

enum class LockMode {
    shared,
    exclusive,
};

enum class LockErrorCode {
    invalid_table_id,
    invalid_transaction_id,
    lock_conflict,
};

struct LockError {
    LockErrorCode code;
    common::TableId table_id;
};

class LockManager {
public:
    LockManager() = default;

    LockManager(const LockManager&) = delete;
    LockManager& operator=(const LockManager&) = delete;
    LockManager(LockManager&&) = delete;
    LockManager& operator=(LockManager&&) = delete;

private:
    friend class TransactionManager;

    struct TableLock {
        std::unordered_set<std::uint64_t> shared_holders;
        std::optional<std::uint64_t> exclusive_holder;
    };

    [[nodiscard]] std::expected<void, LockError> acquire(
        common::TransactionId transaction_id, common::TableId table_id,
        LockMode mode);
    void release_all(common::TransactionId transaction_id);

    std::unordered_map<std::uint64_t, TableLock> table_locks_;
};

}  // namespace minidb::transaction
