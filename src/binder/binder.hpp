#pragma once

#include "binder/bound_statement.hpp"
#include "sql/ast.hpp"

#include <expected>

namespace minidb::binder {

enum class BindErrorCode {
    invalid_integer_literal,
    invalid_string_literal,
    integer_out_of_range,
};

struct BindError {
    BindErrorCode code;
    sql::SourceLocation location;
};

[[nodiscard]] std::expected<BoundSelectStatement, BindError> bind_select_statement(
    const sql::SelectStatement& statement);

}  // namespace minidb::binder
