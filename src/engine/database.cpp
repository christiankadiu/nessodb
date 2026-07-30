#include "engine/database.hpp"

#include "binder/binder.hpp"
#include "sql/parser.hpp"

#include <stdexcept>
#include <utility>
#include <variant>

namespace minidb::engine {

std::expected<QueryResult, QueryError> Database::execute(std::string_view source) {
    sql::Parser parser{source};
    auto parsed = parser.parse_statement();
    if (!parsed) {
        return std::unexpected(QueryError{parsed.error()});
    }

    return std::visit([this](const auto& statement) { return execute(statement); }, *parsed);
}

std::expected<QueryResult, QueryError> Database::execute(
    const sql::SelectStatement& statement) {
    auto bound = binder::bind_select_statement(statement);
    if (!bound) {
        return std::unexpected(QueryError{bound.error()});
    }

    ResultRow row;
    row.values.reserve(bound->expressions.size());
    for (const auto& expression : bound->expressions) {
        row.values.push_back(expression.value);
    }
    QueryResult result;
    result.rows.push_back(std::move(row));
    return result;
}

std::expected<QueryResult, QueryError> Database::execute(
    const sql::CreateTableStatement& statement) {
    auto bound = binder::bind_create_table_statement(statement);
    if (!bound) {
        return std::unexpected(QueryError{bound.error()});
    }

    catalog::TableSchema schema;
    schema.name = std::move(bound->table_name);
    schema.columns.reserve(bound->columns.size());
    for (auto& column : bound->columns) {
        schema.columns.push_back(catalog::ColumnSchema{std::move(column.name), column.type});
    }

    auto created = catalog_.create_table(std::move(schema));
    if (!created) {
        return std::unexpected(QueryError{ExecutionError{
            ExecutionErrorCode::table_already_exists, statement.table_location}});
    }

    auto heap_created = heap_.create_table((*created)->id);
    if (!heap_created) {
        throw std::logic_error{"catalog and heap table state diverged"};
    }
    return QueryResult{};
}

std::expected<QueryResult, QueryError> Database::execute(
    const sql::InsertStatement& statement) {
    auto bound = binder::bind_insert_statement(statement, catalog_);
    if (!bound) {
        return std::unexpected(QueryError{bound.error()});
    }

    auto inserted = heap_.insert(bound->table_id, storage::Row{std::move(bound->values)});
    if (!inserted) {
        throw std::logic_error{"catalog and heap table state diverged"};
    }
    return QueryResult{{}, 1};
}

}  // namespace minidb::engine
