#include "binder/binder.hpp"

#include <charconv>
#include <cstdint>
#include <system_error>

namespace minidb::binder {

std::expected<BoundSelectStatement, BindError> bind_select_statement(
    const sql::SelectStatement& statement) {
    BoundSelectStatement bound_statement;
    bound_statement.expressions.reserve(statement.expressions.size());

    for (const auto& expression : statement.expressions) {
        if (expression.type != sql::LiteralType::integer) {
            return std::unexpected(
                BindError{BindErrorCode::unsupported_literal, expression.location});
        }

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

        bound_statement.expressions.push_back(BoundIntegerExpression{value, expression.location});
    }

    return bound_statement;
}

}  // namespace minidb::binder
