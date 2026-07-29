#include "binder/binder.hpp"

#include <charconv>
#include <cctype>
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

bool identifiers_equal(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto left_character = static_cast<unsigned char>(left[index]);
        const auto right_character = static_cast<unsigned char>(right[index]);
        if (std::tolower(left_character) != std::tolower(right_character)) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::expected<BoundSelectStatement, BindError> bind_select_statement(
    const sql::SelectStatement& statement) {
    BoundSelectStatement bound_statement;
    bound_statement.expressions.reserve(statement.expressions.size());

    for (const auto& expression : statement.expressions) {
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
        bound_statement.expressions.push_back(
            BoundLiteralExpression{std::move(value), expression.location});
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
            if (identifiers_equal(column.name, existing_column.name)) {
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

}  // namespace minidb::binder
