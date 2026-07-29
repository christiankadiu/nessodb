#pragma once

#include "sql/token.hpp"

#include <string_view>
#include <vector>

namespace minidb::sql {

struct IntegerLiteralExpression {
    std::string_view text;
    SourceLocation location;
};

struct SelectStatement {
    std::vector<IntegerLiteralExpression> expressions;
};

}  // namespace minidb::sql
