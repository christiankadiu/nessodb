#pragma once

#include "transaction/lock_manager.hpp"
#include "transaction/transaction.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <variant>

namespace nessodb::transaction {

using TransactionHandle = std::shared_ptr<const Transaction>;
using TransactionLockError = std::variant<TransactionError, LockError>;

class TransactionManager {
public:
    TransactionManager() = default;
    ~TransactionManager();

    TransactionManager(const TransactionManager&) = delete;
    TransactionManager& operator=(const TransactionManager&) = delete;
    TransactionManager(TransactionManager&&) = delete;
    TransactionManager& operator=(TransactionManager&&) = delete;

    [[nodiscard]] std::expected<TransactionHandle, TransactionError> begin();
    [[nodiscard]] std::expected<void, TransactionError> commit(
        const TransactionHandle& transaction);
    [[nodiscard]] std::expected<void, TransactionError> rollback(
        const TransactionHandle& transaction);
    [[nodiscard]] std::expected<void, TransactionLockError> acquire_table_lock(
        const TransactionHandle& transaction, common::TableId table_id,
        LockMode mode);

    [[nodiscard]] std::size_t active_transaction_count() const;

private:
    [[nodiscard]] std::expected<std::shared_ptr<Transaction>, TransactionError>
    find_active(const TransactionHandle& transaction) const;

    mutable std::mutex mutex_;
    std::uint64_t next_transaction_id_{1};
    std::unordered_map<std::uint64_t, std::shared_ptr<Transaction>> active_;
    LockManager lock_manager_;
};

}  // namespace nessodb::transaction
