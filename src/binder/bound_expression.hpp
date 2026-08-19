#pragma once

#include "sql/token.hpp"
#include "types/logical_type.hpp"
#include "types/value.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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
    std::optional<types::LogicalType> value_type{};
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

enum class BoundExpressionResultType {
    value,
    boolean,
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
private:
    BoundExpressionResultType result_type_;

public:
    BoundExpression(BoundLiteralExpression expression)
        : result_type_(BoundExpressionResultType::value),
          node(std::move(expression)) {}
    BoundExpression(BoundColumnReferenceExpression expression)
        : result_type_(BoundExpressionResultType::value),
          node(std::move(expression)) {}
    BoundExpression(BoundComparisonExpression expression)
        : result_type_(BoundExpressionResultType::boolean),
          node(std::move(expression)) {}
    BoundExpression(BoundNullTestExpression expression)
        : result_type_(BoundExpressionResultType::boolean),
          node(std::move(expression)) {}
    BoundExpression(BoundLogicalExpression expression)
        : result_type_(BoundExpressionResultType::boolean),
          node(std::move(expression)) {}
    BoundExpression(BoundNegationExpression expression)
        : result_type_(BoundExpressionResultType::boolean),
          node(std::move(expression)) {}

    [[nodiscard]] BoundExpressionResultType result_type() const noexcept {
        return result_type_;
    }

    [[nodiscard]] std::optional<types::LogicalType> value_type() const noexcept {
        if (const auto* literal =
                std::get_if<BoundLiteralExpression>(&node)) {
            if (std::holds_alternative<std::int64_t>(literal->value)) {
                return types::LogicalType::integer;
            }
            if (std::holds_alternative<std::string>(literal->value)) {
                return types::LogicalType::text;
            }
            return std::nullopt;
        }
        if (const auto* column =
                std::get_if<BoundColumnReferenceExpression>(&node)) {
            return column->value_type;
        }
        return std::nullopt;
    }

    BoundExpressionNode node;
};

}  // namespace minidb::binder
