#pragma once

#include "sql/token.hpp"

#include <cstdint>
#include <vector>

namespace minidb::binder {

struct BoundIntegerExpression {
    std::int64_t value{};
    sql::SourceLocation location;
};

struct BoundSelectStatement {
    std::vector<BoundIntegerExpression> expressions;
};

}  // namespace minidb::binder
