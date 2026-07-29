#pragma once

#include "binder/binder.hpp"
#include "sql/parser.hpp"
#include "types/value.hpp"

#include <cstddef>
#include <expected>
#include <string_view>
#include <variant>
#include <vector>

namespace minidb::engine {

struct QueryResult {
    std::vector<types::Value> values;
    std::size_t rows_affected{};
};

enum class ExecutionErrorCode {
    table_already_exists,
};

struct ExecutionError {
    ExecutionErrorCode code;
    sql::SourceLocation location;
};

using QueryError = std::variant<sql::ParseError, binder::BindError, ExecutionError>;

[[nodiscard]] std::expected<QueryResult, QueryError> execute_query(std::string_view source);

}  // namespace minidb::engine
