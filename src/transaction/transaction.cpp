#include "transaction/transaction.hpp"

#include <utility>

namespace minidb::transaction {

std::expected<Transaction, TransactionError> Transaction::start(
    common::TransactionId id) noexcept {
    if (!id.is_valid()) {
        return std::unexpected(TransactionError{
            TransactionErrorCode::invalid_transaction_id});
    }
    return Transaction{id};
}

Transaction::Transaction(common::TransactionId id) noexcept : id_(id) {}

Transaction::Transaction(Transaction&& other) noexcept
    : id_(std::exchange(other.id_, common::TransactionId{})),
      state_(std::exchange(other.state_, TransactionState::aborted)) {}

common::TransactionId Transaction::id() const noexcept {
    return id_;
}

TransactionState Transaction::state() const noexcept {
    return state_;
}

bool Transaction::is_active() const noexcept {
    return state_ == TransactionState::active;
}

std::expected<void, TransactionError> Transaction::commit() noexcept {
    if (!is_active()) {
        return std::unexpected(TransactionError{
            TransactionErrorCode::transaction_not_active});
    }
    state_ = TransactionState::committed;
    return {};
}

std::expected<void, TransactionError> Transaction::abort() noexcept {
    if (!is_active()) {
        return std::unexpected(TransactionError{
            TransactionErrorCode::transaction_not_active});
    }
    state_ = TransactionState::aborted;
    return {};
}

}  // namespace minidb::transaction
