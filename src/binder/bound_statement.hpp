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

enum class BoundComparisonOperator {
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
};

struct BoundComparisonPredicate {
    std::size_t column_index;
    BoundComparisonOperator comparison;
    types::Value value;
};

struct BoundNullPredicate {
    std::size_t column_index;
    bool negated{};
};

using BoundPredicate =
    std::variant<BoundComparisonPredicate, BoundNullPredicate>;

struct BoundSelectStatement {
    std::vector<BoundSelectExpression> expressions;
    common::TableId table_id;
    std::optional<BoundPredicate> where;
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
