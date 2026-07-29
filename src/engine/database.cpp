#include "engine/database.hpp"

#include "binder/binder.hpp"
#include "sql/parser.hpp"

#include <utility>
#include <variant>

namespace minidb::engine {

std::expected<QueryResult, QueryError> Database::execute(std::string_view source) {
    sql::Parser parser{source};
    auto parsed = parser.parse_statement();
    if (!parsed) {
        return std::unexpected(QueryError{parsed.error()});
    }

    if (const auto* select = std::get_if<sql::SelectStatement>(&*parsed)) {
        return execute(*select);
    }
    return execute(std::get<sql::CreateTableStatement>(*parsed));
}

std::expected<QueryResult, QueryError> Database::execute(
    const sql::SelectStatement& statement) {
    auto bound = binder::bind_select_statement(statement);
    if (!bound) {
        return std::unexpected(QueryError{bound.error()});
    }

    QueryResult result;
    result.values.reserve(bound->expressions.size());
    for (const auto& expression : bound->expressions) {
        result.values.push_back(expression.value);
    }
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
    return QueryResult{};
}

}  // namespace minidb::engine
