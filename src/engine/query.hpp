#pragma once

#include "binder/binder.hpp"
#include "sql/parser.hpp"
#include "storage/storage_manager.hpp"
#include "types/value.hpp"

#include <cstddef>
#include <expected>
#include <string_view>
#include <variant>
#include <vector>

namespace minidb::engine {

struct ResultRow {
    std::vector<types::Value> values;
};

struct QueryResult {
    std::vector<ResultRow> rows;
    std::size_t rows_affected{};
};

enum class ExecutionErrorCode {
    table_already_exists,
    memory_limit_exceeded,
};

struct ExecutionError {
    ExecutionErrorCode code;
    sql::SourceLocation location;
};

struct StorageError {
    storage::StorageManagerError cause;
};

using QueryError =
    std::variant<sql::ParseError, binder::BindError, ExecutionError, StorageError>;

[[nodiscard]] std::expected<QueryResult, QueryError> execute_query(std::string_view source);

}  // namespace minidb::engine
