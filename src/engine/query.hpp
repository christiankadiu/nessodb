#pragma once

#include "binder/binder.hpp"
#include "recovery/wal.hpp"
#include "sql/parser.hpp"
#include "storage/storage_manager.hpp"
#include "types/value.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace minidb::engine {

struct ResultRow {
    std::vector<types::Value> values;
};

struct QueryResult {
    std::vector<std::string> column_names;
    std::vector<ResultRow> rows;
    std::size_t rows_affected{};
};

enum class ExecutionErrorCode {
    table_already_exists,
    memory_limit_exceeded,
    integer_overflow,
    division_by_zero,
    type_mismatch,
};

struct ExecutionError {
    ExecutionErrorCode code;
    sql::SourceLocation location;
};

struct StorageError {
    storage::StorageManagerError cause;
};

struct RecoveryError {
    recovery::WalError cause;
};

enum class TransactionExecutionErrorCode {
    transaction_already_active,
    no_active_transaction,
    lock_conflict,
    rollback_failed,
    transaction_state_error,
    ddl_not_supported,
};

struct TransactionExecutionError {
    TransactionExecutionErrorCode code;
};

using QueryError =
    std::variant<sql::ParseError, binder::BindError, ExecutionError, StorageError,
                 RecoveryError, TransactionExecutionError>;

[[nodiscard]] std::expected<QueryResult, QueryError> execute_query(std::string_view source);

}  // namespace minidb::engine
