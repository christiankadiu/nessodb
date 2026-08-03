#pragma once

#include "common/table_id.hpp"
#include "sql/token.hpp"
#include "types/logical_type.hpp"
#include "types/value.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace minidb::binder {

struct BoundLiteralExpression {
    types::Value value;
    sql::SourceLocation location;
};

struct BoundColumnReferenceExpression {
    std::size_t column_index;
    sql::SourceLocation location;
};

using BoundSelectExpression =
    std::variant<BoundLiteralExpression, BoundColumnReferenceExpression>;

struct BoundEqualityPredicate {
    std::size_t column_index;
    types::Value value;
};

struct BoundSelectStatement {
    std::vector<BoundSelectExpression> expressions;
    common::TableId table_id;
    std::optional<BoundEqualityPredicate> where;
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

struct BoundInsertStatement {
    common::TableId table_id;
    std::vector<types::Value> values;
};

}  // namespace minidb::binder
