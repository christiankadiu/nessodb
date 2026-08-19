#pragma once

#include "binder/bound_statement.hpp"
#include "types/value.hpp"

#include <span>

namespace minidb::execution {

[[nodiscard]] bool matches_predicate(
    std::span<const types::Value> values,
    const binder::BoundExpression& predicate);

}  // namespace minidb::execution
