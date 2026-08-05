#pragma once

#include "common/table_id.hpp"
#include "sql/token.hpp"
#include "types/logical_type.hpp"
#include "types/value.hpp"

#include <cstddef>
#include <memory>
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

struct BoundLogicalPredicate;
struct BoundNegationPredicate;

using BoundPredicate =
    std::variant<BoundComparisonPredicate, BoundNullPredicate,
                 std::unique_ptr<BoundLogicalPredicate>,
                 std::unique_ptr<BoundNegationPredicate>>;

enum class BoundLogicalOperator {
    conjunction,
    disjunction,
};

struct BoundLogicalPredicate {
    BoundLogicalOperator operation;
    BoundPredicate left;
    BoundPredicate right;
};

struct BoundNegationPredicate {
    BoundPredicate operand;
};

enum class BoundOrderDirection {
    ascending,
    descending,
};

struct BoundOrderByTerm {
    std::size_t column_index;
    BoundOrderDirection direction;
};

struct BoundSelectStatement {
    std::vector<BoundSelectExpression> expressions;
    common::TableId table_id;
    std::optional<BoundPredicate> where;
    std::vector<BoundOrderByTerm> order_by;
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

struct BoundDeleteStatement {
    common::TableId table_id;
    std::optional<BoundPredicate> where;
};

struct BoundUpdateAssignment {
    std::size_t column_index;
    types::Value value;
};

struct BoundUpdateStatement {
    common::TableId table_id;
    std::vector<BoundUpdateAssignment> assignments;
    std::optional<BoundPredicate> where;
};

}  // namespace minidb::binder
