#pragma once

#include "binder/bound_expression.hpp"
#include "types/value.hpp"

#include <span>

namespace minidb::execution {

[[nodiscard]] types::Value evaluate_expression(
    std::span<const types::Value> input,
    const binder::BoundExpression& expression);

}  // namespace minidb::execution
