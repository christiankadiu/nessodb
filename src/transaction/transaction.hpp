#pragma once

#include "common/transaction_id.hpp"

#include <expected>

namespace minidb::transaction {

class TransactionManager;

enum class TransactionState {
    active,
    committed,
    aborted,
};

enum class TransactionErrorCode {
    invalid_transaction_id,
    transaction_id_exhausted,
    transaction_not_active,
    transaction_not_owned,
};

struct TransactionError {
    TransactionErrorCode code;
};

class Transaction {
public:
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&&) = delete;

    [[nodiscard]] common::TransactionId id() const noexcept;
    [[nodiscard]] TransactionState state() const noexcept;
    [[nodiscard]] bool is_active() const noexcept;

private:
    friend class TransactionManager;

    [[nodiscard]] static std::expected<Transaction, TransactionError> start(
        common::TransactionId id) noexcept;
    explicit Transaction(common::TransactionId id) noexcept;

    [[nodiscard]] std::expected<void, TransactionError> commit() noexcept;
    [[nodiscard]] std::expected<void, TransactionError> abort() noexcept;

    common::TransactionId id_;
    TransactionState state_{TransactionState::active};
};

}  // namespace minidb::transaction
