#pragma once

#include "binder/bound_statement.hpp"
#include "sql/ast.hpp"

#include <expected>

namespace minidb::binder {

enum class BindErrorCode {
    invalid_integer_literal,
    invalid_string_literal,
    integer_out_of_range,
    duplicate_column,
};

struct BindError {
    BindErrorCode code;
    sql::SourceLocation location;
};

[[nodiscard]] std::expected<BoundSelectStatement, BindError> bind_select_statement(
    const sql::SelectStatement& statement);
[[nodiscard]] std::expected<BoundCreateTableStatement, BindError> bind_create_table_statement(
    const sql::CreateTableStatement& statement);

}  // namespace minidb::binder
