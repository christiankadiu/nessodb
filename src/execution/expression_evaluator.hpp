#pragma once

#include "binder/bound_expression.hpp"
#include "types/value.hpp"

#include <expected>
#include <span>
#include <variant>

namespace nessodb::execution {

using ExpressionResult = std::variant<types::Value, bool>;

enum class ExpressionErrorCode {
    integer_overflow,
    division_by_zero,
};

struct ExpressionError {
    ExpressionErrorCode code;
    sql::SourceLocation location;
};

using ExpressionEvaluation =
    std::expected<ExpressionResult, ExpressionError>;

[[nodiscard]] ExpressionEvaluation evaluate_expression(
    std::span<const types::Value> input,
    const binder::BoundExpression& expression);

}  // namespace nessodb::execution
