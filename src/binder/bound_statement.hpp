#pragma once

#include "sql/token.hpp"
#include "types/value.hpp"

#include <vector>

namespace minidb::binder {

struct BoundLiteralExpression {
    types::Value value;
    sql::SourceLocation location;
};

struct BoundSelectStatement {
    std::vector<BoundLiteralExpression> expressions;
};

}  // namespace minidb::binder
