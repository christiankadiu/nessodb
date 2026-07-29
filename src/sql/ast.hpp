#pragma once

#include "sql/token.hpp"

#include <string_view>
#include <vector>

namespace minidb::sql {

enum class LiteralType {
    integer,
    string,
};

struct LiteralExpression {
    LiteralType type;
    std::string_view text;
    SourceLocation location;
};

struct SelectStatement {
    std::vector<LiteralExpression> expressions;
};

}  // namespace minidb::sql
