#include "transaction/lock_manager.hpp"

namespace nessodb::transaction {

std::expected<void, LockError> LockManager::acquire(
    common::TransactionId transaction_id, common::TableId table_id,
    LockMode mode) {
    if (!transaction_id.is_valid()) {
        return std::unexpected(
            LockError{LockErrorCode::invalid_transaction_id, table_id});
    }
    if (!table_id.is_valid()) {
        return std::unexpected(
            LockError{LockErrorCode::invalid_table_id, table_id});
    }

    auto& lock = table_locks_[table_id.value];
    if (mode == LockMode::shared) {
        if (lock.exclusive_holder &&
            *lock.exclusive_holder != transaction_id.value) {
            return std::unexpected(
                LockError{LockErrorCode::lock_conflict, table_id});
        }
        if (!lock.exclusive_holder) {
            lock.shared_holders.insert(transaction_id.value);
        }
        return {};
    }

    if (lock.exclusive_holder) {
        if (*lock.exclusive_holder == transaction_id.value) {
            return {};
        }
        return std::unexpected(
            LockError{LockErrorCode::lock_conflict, table_id});
    }
    if (!lock.shared_holders.empty() &&
        (lock.shared_holders.size() != 1 ||
         !lock.shared_holders.contains(transaction_id.value))) {
        return std::unexpected(
            LockError{LockErrorCode::lock_conflict, table_id});
    }

    lock.shared_holders.erase(transaction_id.value);
    lock.exclusive_holder = transaction_id.value;
    return {};
}

void LockManager::release_all(common::TransactionId transaction_id) {
    for (auto entry = table_locks_.begin(); entry != table_locks_.end();) {
        auto& lock = entry->second;
        lock.shared_holders.erase(transaction_id.value);
        if (lock.exclusive_holder == transaction_id.value) {
            lock.exclusive_holder.reset();
        }

        if (lock.shared_holders.empty() && !lock.exclusive_holder) {
            entry = table_locks_.erase(entry);
        } else {
            ++entry;
        }
    }
}

}  // namespace nessodb::transaction
