#include "binder/binder.hpp"

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

}  // namespace minidb::binder
