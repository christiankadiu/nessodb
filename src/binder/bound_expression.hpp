#pragma once

#include "sql/token.hpp"
#include "types/value.hpp"

#include <cstddef>
#include <memory>
#include <utility>
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

enum class BoundComparisonOperator {
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
};

enum class BoundLogicalOperator {
    conjunction,
    disjunction,
};

struct BoundExpression;
using BoundExpressionPtr = std::unique_ptr<BoundExpression>;

struct BoundComparisonExpression {
    BoundComparisonOperator comparison;
    BoundExpressionPtr left;
    BoundExpressionPtr right;
};

struct BoundNullTestExpression {
    BoundExpressionPtr operand;
    bool negated{};
};

struct BoundLogicalExpression {
    BoundLogicalOperator operation;
    BoundExpressionPtr left;
    BoundExpressionPtr right;
};

struct BoundNegationExpression {
    BoundExpressionPtr operand;
};

using BoundExpressionNode =
    std::variant<BoundLiteralExpression, BoundColumnReferenceExpression,
                 BoundComparisonExpression, BoundNullTestExpression,
                 BoundLogicalExpression, BoundNegationExpression>;

struct BoundExpression {
    BoundExpression(BoundLiteralExpression expression)
        : node(std::move(expression)) {}
    BoundExpression(BoundColumnReferenceExpression expression)
        : node(std::move(expression)) {}
    BoundExpression(BoundComparisonExpression expression)
        : node(std::move(expression)) {}
    BoundExpression(BoundNullTestExpression expression)
        : node(std::move(expression)) {}
    BoundExpression(BoundLogicalExpression expression)
        : node(std::move(expression)) {}
    BoundExpression(BoundNegationExpression expression)
        : node(std::move(expression)) {}

    BoundExpressionNode node;
};

}  // namespace minidb::binder
