#pragma once

#include "sql/token.hpp"
#include "types/value.hpp"

#include <cstddef>
#include <variant>

namespace minidb::binder {

struct BoundLiteralExpression {
    types::Value value;
    sql::SourceLocation location;
};

struct BoundColumnReferenceExpression {
    std::size_t column_index;
    sql::SourceLocation location;
};

using BoundExpression =
    std::variant<BoundLiteralExpression, BoundColumnReferenceExpression>;

}  // namespace minidb::binder
