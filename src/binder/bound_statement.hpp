#pragma once

#include "sql/token.hpp"
#include "types/logical_type.hpp"
#include "types/value.hpp"

#include <string>
#include <vector>

namespace minidb::binder {

struct BoundLiteralExpression {
    types::Value value;
    sql::SourceLocation location;
};

struct BoundSelectStatement {
    std::vector<BoundLiteralExpression> expressions;
};

struct BoundColumnDefinition {
    std::string name;
    types::LogicalType type;
    sql::SourceLocation location;
};

struct BoundCreateTableStatement {
    std::string table_name;
    sql::SourceLocation table_location;
    std::vector<BoundColumnDefinition> columns;
};

}  // namespace minidb::binder
