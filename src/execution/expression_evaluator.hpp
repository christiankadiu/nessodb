#pragma once

#include "binder/bound_expression.hpp"
#include "types/value.hpp"

#include <span>
#include <variant>

namespace minidb::execution {

using ExpressionResult = std::variant<types::Value, bool>;

[[nodiscard]] ExpressionResult evaluate_expression(
    std::span<const types::Value> input,
    const binder::BoundExpression& expression);

}  // namespace minidb::execution
