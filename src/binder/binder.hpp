#pragma once

#include "binder/bound_statement.hpp"
#include "sql/ast.hpp"

#include <expected>

namespace nessodb::catalog {

class Catalog;

}  // namespace nessodb::catalog

namespace nessodb::binder {

enum class BindErrorCode {
    invalid_integer_literal,
    invalid_string_literal,
    integer_out_of_range,
    duplicate_column,
    multiple_primary_keys,
    table_not_found,
    column_requires_table,
    column_not_found,
    column_count_mismatch,
    type_mismatch,
    function_not_found,
    invalid_function_arguments,
    mixed_aggregate_and_scalar,
    column_not_grouped,
    ambiguous_column,
    duplicate_table,
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
[[nodiscard]] std::expected<BoundDeleteStatement, BindError> bind_delete_statement(
    const sql::DeleteStatement& statement, const catalog::Catalog& catalog);
[[nodiscard]] std::expected<BoundUpdateStatement, BindError> bind_update_statement(
    const sql::UpdateStatement& statement, const catalog::Catalog& catalog);

}  // namespace nessodb::binder
