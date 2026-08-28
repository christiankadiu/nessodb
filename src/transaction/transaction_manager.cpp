#include "transaction/transaction_manager.hpp"

#include <limits>
#include <utility>

namespace minidb::transaction {

TransactionManager::~TransactionManager() {
    const std::scoped_lock lock{mutex_};
    for (const auto& [id, transaction] : active_) {
        (void)transaction->abort();
        lock_manager_.release_all(common::TransactionId{id});
    }
}

std::expected<TransactionHandle, TransactionError> TransactionManager::begin() {
    const std::scoped_lock lock{mutex_};
    if (next_transaction_id_ == 0) {
        return std::unexpected(TransactionError{
            TransactionErrorCode::transaction_id_exhausted});
    }

    const common::TransactionId id{next_transaction_id_};
    if (next_transaction_id_ == std::numeric_limits<std::uint64_t>::max()) {
        next_transaction_id_ = 0;
    } else {
        ++next_transaction_id_;
    }

    auto started = Transaction::start(id);
    if (!started) {
        return std::unexpected(started.error());
    }

    auto transaction = std::make_shared<Transaction>(std::move(*started));
    active_.emplace(id.value, transaction);
    return TransactionHandle{std::move(transaction)};
}

std::expected<void, TransactionError> TransactionManager::commit(
    const TransactionHandle& transaction) {
    const std::scoped_lock lock{mutex_};
    auto active = find_active(transaction);
    if (!active) {
        return std::unexpected(active.error());
    }

    auto committed = (*active)->commit();
    if (!committed) {
        return std::unexpected(committed.error());
    }
    lock_manager_.release_all((*active)->id());
    active_.erase((*active)->id().value);
    return {};
}

std::expected<void, TransactionError> TransactionManager::rollback(
    const TransactionHandle& transaction) {
    const std::scoped_lock lock{mutex_};
    auto active = find_active(transaction);
    if (!active) {
        return std::unexpected(active.error());
    }

    auto aborted = (*active)->abort();
    if (!aborted) {
        return std::unexpected(aborted.error());
    }
    lock_manager_.release_all((*active)->id());
    active_.erase((*active)->id().value);
    return {};
}

std::expected<void, TransactionLockError>
TransactionManager::acquire_table_lock(
    const TransactionHandle& transaction, common::TableId table_id,
    LockMode mode) {
    const std::scoped_lock lock{mutex_};
    auto active = find_active(transaction);
    if (!active) {
        return std::unexpected(TransactionLockError{active.error()});
    }

    auto acquired = lock_manager_.acquire((*active)->id(), table_id, mode);
    if (!acquired) {
        return std::unexpected(TransactionLockError{acquired.error()});
    }
    return {};
}

std::size_t TransactionManager::active_transaction_count() const {
    const std::scoped_lock lock{mutex_};
    return active_.size();
}

std::expected<std::shared_ptr<Transaction>, TransactionError>
TransactionManager::find_active(
    const TransactionHandle& transaction) const {
    if (!transaction) {
        return std::unexpected(TransactionError{
            TransactionErrorCode::transaction_not_owned});
    }
    if (!transaction->is_active()) {
        return std::unexpected(TransactionError{
            TransactionErrorCode::transaction_not_active});
    }

    const auto entry = active_.find(transaction->id().value);
    if (entry == active_.end() || entry->second.get() != transaction.get()) {
        return std::unexpected(TransactionError{
            TransactionErrorCode::transaction_not_owned});
    }
    return entry->second;
}

}  // namespace minidb::transaction
