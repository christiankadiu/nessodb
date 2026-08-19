#include "execution/expression_evaluator.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>

namespace minidb::execution {
namespace {

const types::Value& require_value(const ExpressionResult& result) {
    const auto* value = std::get_if<types::Value>(&result);
    if (value == nullptr) {
        throw std::logic_error{
            "bound expression operand does not produce a stored value"};
    }
    return *value;
}

bool require_boolean(const ExpressionResult& result) {
    const auto* value = std::get_if<bool>(&result);
    if (value == nullptr) {
        throw std::logic_error{
            "bound expression operand does not produce a boolean"};
    }
    return *value;
}

bool evaluate_comparison(const types::Value& left,
                         const types::Value& right,
                         binder::BoundComparisonOperator comparison) {
    if (std::holds_alternative<types::NullValue>(left) ||
        std::holds_alternative<types::NullValue>(right)) {
        return false;
    }
    if (left.index() != right.index()) {
        throw std::logic_error{
            "bound expression and stored value types diverged"};
    }

    if (comparison == binder::BoundComparisonOperator::equal) {
        return left == right;
    }
    if (comparison == binder::BoundComparisonOperator::not_equal) {
        return left != right;
    }

    bool less = false;
    bool greater = false;
    if (const auto* left_integer = std::get_if<std::int64_t>(&left)) {
        const auto right_integer = std::get<std::int64_t>(right);
        less = *left_integer < right_integer;
        greater = *left_integer > right_integer;
    } else {
        const auto& left_text = std::get<std::string>(left);
        const auto& right_text = std::get<std::string>(right);
        less = left_text < right_text;
        greater = left_text > right_text;
    }

    switch (comparison) {
        case binder::BoundComparisonOperator::less:
            return less;
        case binder::BoundComparisonOperator::less_equal:
            return !greater;
        case binder::BoundComparisonOperator::greater:
            return greater;
        case binder::BoundComparisonOperator::greater_equal:
            return !less;
        case binder::BoundComparisonOperator::equal:
        case binder::BoundComparisonOperator::not_equal:
            break;
    }
    throw std::logic_error{"unknown bound comparison operator"};
}

}  // namespace

ExpressionResult evaluate_expression(
    std::span<const types::Value> input,
    const binder::BoundExpression& expression) {
    if (const auto* literal =
            std::get_if<binder::BoundLiteralExpression>(&expression.node)) {
        return literal->value;
    }

    if (const auto* column =
            std::get_if<binder::BoundColumnReferenceExpression>(
                &expression.node)) {
        const auto column_index = column->column_index;
        if (column_index >= input.size()) {
            throw std::logic_error{
                "table schema and stored row state diverged"};
        }
        return input[column_index];
    }

    if (const auto* comparison =
            std::get_if<binder::BoundComparisonExpression>(
                &expression.node)) {
        if (!comparison->left || !comparison->right) {
            throw std::logic_error{
                "comparison expression requires two operands"};
        }
        const auto left = evaluate_expression(input, *comparison->left);
        const auto right = evaluate_expression(input, *comparison->right);
        return evaluate_comparison(require_value(left), require_value(right),
                                   comparison->comparison);
    }

    if (const auto* null_test =
            std::get_if<binder::BoundNullTestExpression>(
                &expression.node)) {
        if (!null_test->operand) {
            throw std::logic_error{
                "null test expression requires an operand"};
        }
        const auto operand = evaluate_expression(input, *null_test->operand);
        const bool is_null = std::holds_alternative<types::NullValue>(
            require_value(operand));
        return null_test->negated ? !is_null : is_null;
    }

    if (const auto* negation =
            std::get_if<binder::BoundNegationExpression>(
                &expression.node)) {
        if (!negation->operand) {
            throw std::logic_error{
                "negation expression requires an operand"};
        }
        return !require_boolean(
            evaluate_expression(input, *negation->operand));
    }

    const auto* logical = std::get_if<binder::BoundLogicalExpression>(
        &expression.node);
    if (logical == nullptr) {
        throw std::logic_error{"unknown bound expression node"};
    }
    if (!logical->left || !logical->right) {
        throw std::logic_error{
            "logical expression requires two operands"};
    }

    const bool left = require_boolean(
        evaluate_expression(input, *logical->left));
    switch (logical->operation) {
        case binder::BoundLogicalOperator::conjunction:
            return left && require_boolean(
                               evaluate_expression(input, *logical->right));
        case binder::BoundLogicalOperator::disjunction:
            return left || require_boolean(
                              evaluate_expression(input, *logical->right));
    }
    throw std::logic_error{"unknown bound logical operator"};
}

}  // namespace minidb::execution
