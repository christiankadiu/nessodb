#pragma once

#include "binder/bound_expression.hpp"
#include "common/table_id.hpp"
#include "sql/token.hpp"
#include "types/logical_type.hpp"
#include "types/value.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace nessodb::binder {

enum class BoundOrderDirection {
    ascending,
    descending,
};

struct BoundOrderByTerm {
    std::size_t column_index;
    BoundOrderDirection direction;
};

enum class BoundAggregateFunction {
    count,
    minimum,
    maximum,
    sum,
};

struct BoundAggregateExpression {
    BoundAggregateFunction function;
    std::vector<BoundExpression> arguments;
    sql::SourceLocation location;
};

struct BoundJoin {
    common::TableId table_id;
    BoundExpression condition;
};

struct BoundPrimaryKeyLookup {
    std::size_t column_index;
    types::Value value;
};

struct BoundSelectStatement {
    std::vector<BoundExpression> expressions;
    std::vector<std::string> result_column_names;
    std::vector<BoundAggregateExpression> aggregates;
    std::vector<BoundExpression> group_by;
    std::vector<BoundJoin> joins;
    common::TableId table_id;
    bool distinct{};
    std::optional<BoundExpression> where;
    std::optional<BoundPrimaryKeyLookup> primary_key_lookup;
    std::vector<BoundOrderByTerm> order_by;
    std::optional<std::size_t> limit;
    std::optional<std::size_t> offset;
};

struct BoundColumnDefinition {
    std::string name;
    types::LogicalType type;
    sql::SourceLocation location;
    bool primary_key{};
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

struct BoundDeleteStatement {
    common::TableId table_id;
    std::optional<BoundExpression> where;
};

struct BoundUpdateAssignment {
    std::size_t column_index;
    types::Value value;
};

struct BoundUpdateStatement {
    common::TableId table_id;
    std::vector<BoundUpdateAssignment> assignments;
    std::optional<BoundExpression> where;
};

}  // namespace nessodb::binder
