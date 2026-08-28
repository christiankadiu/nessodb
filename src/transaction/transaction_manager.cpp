#include "transaction/transaction_manager.hpp"

#include <limits>
#include <utility>

namespace minidb::transaction {

TransactionManager::~TransactionManager() {
    for (const auto& [id, transaction] : active_) {
        (void)id;
        (void)transaction->abort();
    }
}

std::expected<TransactionHandle, TransactionError> TransactionManager::begin() {
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
    auto active = find_active(transaction);
    if (!active) {
        return std::unexpected(active.error());
    }

    auto committed = (*active)->commit();
    if (!committed) {
        return std::unexpected(committed.error());
    }
    active_.erase((*active)->id().value);
    return {};
}

std::expected<void, TransactionError> TransactionManager::rollback(
    const TransactionHandle& transaction) {
    auto active = find_active(transaction);
    if (!active) {
        return std::unexpected(active.error());
    }

    auto aborted = (*active)->abort();
    if (!aborted) {
        return std::unexpected(aborted.error());
    }
    active_.erase((*active)->id().value);
    return {};
}

std::size_t TransactionManager::active_transaction_count() const noexcept {
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
