#include "binder/binder.hpp"

#include "catalog/catalog.hpp"
#include "common/identifier.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace minidb::binder {
namespace {

std::expected<types::Value, BindError> bind_integer(const sql::LiteralExpression& expression) {
    std::int64_t value{};
    const char* const begin = expression.text.data();
    const char* const end = begin + expression.text.size();
    const auto result = std::from_chars(begin, end, value);

    if (result.ec == std::errc::result_out_of_range) {
        return std::unexpected(
            BindError{BindErrorCode::integer_out_of_range, expression.location});
    }
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::unexpected(
            BindError{BindErrorCode::invalid_integer_literal, expression.location});
    }
    return types::Value{value};
}

std::expected<types::Value, BindError> bind_string(const sql::LiteralExpression& expression) {
    const std::string_view text = expression.text;
    if (text.size() < 2 || text.front() != '\'' || text.back() != '\'') {
        return std::unexpected(
            BindError{BindErrorCode::invalid_string_literal, expression.location});
    }

    std::string value;
    value.reserve(text.size() - 2);

    for (std::size_t index = 1; index + 1 < text.size(); ++index) {
        if (text[index] != '\'') {
            value.push_back(text[index]);
            continue;
        }
        if (index + 1 >= text.size() - 1 || text[index + 1] != '\'') {
            return std::unexpected(
                BindError{BindErrorCode::invalid_string_literal, expression.location});
        }
        value.push_back('\'');
        ++index;
    }

    return types::Value{std::move(value)};
}

std::expected<BoundLiteralExpression, BindError> bind_literal(
    const sql::LiteralExpression& expression) {
    types::Value value{types::NullValue{}};
    if (expression.type != sql::LiteralType::null) {
        auto bound_value = expression.type == sql::LiteralType::integer
                               ? bind_integer(expression)
                               : bind_string(expression);
        if (!bound_value) {
            return std::unexpected(bound_value.error());
        }
        value = std::move(*bound_value);
    }
    return BoundLiteralExpression{std::move(value), expression.location};
}

bool value_matches_type(const types::Value& value, types::LogicalType type) noexcept {
    if (std::holds_alternative<types::NullValue>(value)) {
        return true;
    }
    if (type == types::LogicalType::integer) {
        return std::holds_alternative<std::int64_t>(value);
    }
    return std::holds_alternative<std::string>(value);
}

}  // namespace

std::expected<BoundSelectStatement, BindError> bind_select_statement(
    const sql::SelectStatement& statement, const catalog::Catalog& catalog) {
    if (statement.select_all_columns) {
        if (!statement.from) {
            return std::unexpected(BindError{BindErrorCode::table_not_found, {}});
        }
        const catalog::TableSchema* table = catalog.find_table(statement.from->name);
        if (table == nullptr) {
            return std::unexpected(
                BindError{BindErrorCode::table_not_found, statement.from->location});
        }
        return BoundSelectStatement{{}, table->id};
    }

    BoundSelectStatement bound_statement;
    bound_statement.expressions.reserve(statement.expressions.size());

    for (const auto& expression : statement.expressions) {
        auto bound_expression = bind_literal(expression);
        if (!bound_expression) {
            return std::unexpected(bound_expression.error());
        }
        bound_statement.expressions.push_back(std::move(*bound_expression));
    }

    return bound_statement;
}

std::expected<BoundCreateTableStatement, BindError> bind_create_table_statement(
    const sql::CreateTableStatement& statement) {
    BoundCreateTableStatement bound_statement{
        std::string{statement.table_name}, statement.table_location, {}};
    bound_statement.columns.reserve(statement.columns.size());

    for (const auto& column : statement.columns) {
        for (const auto& existing_column : bound_statement.columns) {
            if (common::identifiers_equal(column.name, existing_column.name)) {
                return std::unexpected(
                    BindError{BindErrorCode::duplicate_column, column.location});
            }
        }

        const types::LogicalType type = column.type == sql::ColumnType::integer
                                            ? types::LogicalType::integer
                                            : types::LogicalType::text;
        bound_statement.columns.push_back(
            BoundColumnDefinition{std::string{column.name}, type, column.location});
    }

    return bound_statement;
}

std::expected<BoundInsertStatement, BindError> bind_insert_statement(
    const sql::InsertStatement& statement, const catalog::Catalog& catalog) {
    const catalog::TableSchema* table = catalog.find_table(statement.table_name);
    if (table == nullptr) {
        return std::unexpected(
            BindError{BindErrorCode::table_not_found, statement.table_location});
    }
    if (statement.values.size() != table->columns.size()) {
        return std::unexpected(
            BindError{BindErrorCode::column_count_mismatch, statement.table_location});
    }

    BoundInsertStatement bound_statement{table->id, {}};
    bound_statement.values.reserve(statement.values.size());
    for (std::size_t index = 0; index < statement.values.size(); ++index) {
        auto expression = bind_literal(statement.values[index]);
        if (!expression) {
            return std::unexpected(expression.error());
        }
        if (!value_matches_type(expression->value, table->columns[index].type)) {
            return std::unexpected(
                BindError{BindErrorCode::type_mismatch, expression->location});
        }
        bound_statement.values.push_back(std::move(expression->value));
    }
    return bound_statement;
}

}  // namespace minidb::binder
