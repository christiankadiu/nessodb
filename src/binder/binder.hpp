#pragma once

#include "binder/bound_statement.hpp"
#include "sql/ast.hpp"

#include <expected>

namespace minidb::catalog {

class Catalog;

}  // namespace minidb::catalog

namespace minidb::binder {

enum class BindErrorCode {
    invalid_integer_literal,
    invalid_string_literal,
    integer_out_of_range,
    duplicate_column,
    table_not_found,
    column_count_mismatch,
    type_mismatch,
};

struct BindError {
    BindErrorCode code;
    sql::SourceLocation location;
};

[[nodiscard]] std::expected<BoundSelectStatement, BindError> bind_select_statement(
    const sql::SelectStatement& statement, const catalog::Catalog& catalog);
[[nodiscard]] std::expected<BoundCreateTableStatement, BindError> bind_create_table_statement(
    const sql::CreateTableStatement& statement);
[[nodiscard]] std::expected<BoundInsertStatement, BindError> bind_insert_statement(
    const sql::InsertStatement& statement, const catalog::Catalog& catalog);

}  // namespace minidb::binder
