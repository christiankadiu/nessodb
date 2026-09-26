#include "execution/expression_evaluator.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace nessodb::execution {
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

std::expected<std::int64_t, ExpressionError> evaluate_unary_integer(
    std::int64_t operand,
    binder::BoundUnaryArithmeticOperator operation,
    sql::SourceLocation location) {
    if (operation == binder::BoundUnaryArithmeticOperator::plus) {
        return operand;
    }
    if (operand == std::numeric_limits<std::int64_t>::min()) {
        return std::unexpected(ExpressionError{
            ExpressionErrorCode::integer_overflow, location});
    }
    return -operand;
}

std::expected<std::int64_t, ExpressionError> evaluate_binary_integers(
    std::int64_t left, std::int64_t right,
    binder::BoundBinaryArithmeticOperator operation,
    sql::SourceLocation location) {
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();

    switch (operation) {
        case binder::BoundBinaryArithmeticOperator::addition:
            if ((right > 0 && left > maximum - right) ||
                (right < 0 && left < minimum - right)) {
                return std::unexpected(ExpressionError{
                    ExpressionErrorCode::integer_overflow, location});
            }
            return left + right;
        case binder::BoundBinaryArithmeticOperator::subtraction:
            if ((right < 0 && left > maximum + right) ||
                (right > 0 && left < minimum + right)) {
                return std::unexpected(ExpressionError{
                    ExpressionErrorCode::integer_overflow, location});
            }
            return left - right;
        case binder::BoundBinaryArithmeticOperator::multiplication:
            if ((left > 0 && right > 0 && left > maximum / right) ||
                (left > 0 && right < 0 && right < minimum / left) ||
                (left < 0 && right > 0 && left < minimum / right) ||
                (left < 0 && right < 0 &&
                 left < maximum / right)) {
                return std::unexpected(ExpressionError{
                    ExpressionErrorCode::integer_overflow, location});
            }
            return left * right;
        case binder::BoundBinaryArithmeticOperator::division:
            if (right == 0) {
                return std::unexpected(ExpressionError{
                    ExpressionErrorCode::division_by_zero, location});
            }
            if (left == minimum && right == -1) {
                return std::unexpected(ExpressionError{
                    ExpressionErrorCode::integer_overflow, location});
            }
            return left / right;
    }
    throw std::logic_error{"unknown bound arithmetic operator"};
}

std::expected<types::Value, ExpressionError> evaluate_unary_arithmetic(
    const types::Value& operand,
    binder::BoundUnaryArithmeticOperator operation,
    sql::SourceLocation location) {
    if (std::holds_alternative<types::NullValue>(operand)) {
        return types::Value{types::NullValue{}};
    }
    const auto* integer = std::get_if<std::int64_t>(&operand);
    if (integer == nullptr) {
        throw std::logic_error{
            "bound arithmetic operand is not an integer"};
    }
    auto result = evaluate_unary_integer(*integer, operation, location);
    if (!result) {
        return std::unexpected(result.error());
    }
    return types::Value{*result};
}

std::expected<types::Value, ExpressionError> evaluate_binary_arithmetic(
    const types::Value& left, const types::Value& right,
    binder::BoundBinaryArithmeticOperator operation,
    sql::SourceLocation location) {
    if (std::holds_alternative<types::NullValue>(left) ||
        std::holds_alternative<types::NullValue>(right)) {
        return types::Value{types::NullValue{}};
    }
    const auto* left_integer = std::get_if<std::int64_t>(&left);
    const auto* right_integer = std::get_if<std::int64_t>(&right);
    if (left_integer == nullptr || right_integer == nullptr) {
        throw std::logic_error{
            "bound arithmetic operands are not integers"};
    }
    auto result = evaluate_binary_integers(
        *left_integer, *right_integer, operation, location);
    if (!result) {
        return std::unexpected(result.error());
    }
    return types::Value{*result};
}

}  // namespace

ExpressionEvaluation evaluate_expression(
    std::span<const types::Value> input,
    const binder::BoundExpression& expression) {
    if (const auto* literal =
            std::get_if<binder::BoundLiteralExpression>(&expression.node)) {
        return ExpressionResult{literal->value};
    }

    if (const auto* column =
            std::get_if<binder::BoundColumnReferenceExpression>(
                &expression.node)) {
        const auto column_index = column->column_index;
        if (column_index >= input.size()) {
            throw std::logic_error{
                "table schema and stored row state diverged"};
        }
        return ExpressionResult{input[column_index]};
    }

    if (const auto* comparison =
            std::get_if<binder::BoundComparisonExpression>(
                &expression.node)) {
        if (!comparison->left || !comparison->right) {
            throw std::logic_error{
                "comparison expression requires two operands"};
        }
        const auto left = evaluate_expression(input, *comparison->left);
        if (!left) {
            return std::unexpected(left.error());
        }
        const auto right = evaluate_expression(input, *comparison->right);
        if (!right) {
            return std::unexpected(right.error());
        }
        return ExpressionResult{evaluate_comparison(
            require_value(*left), require_value(*right),
            comparison->comparison)};
    }

    if (const auto* null_test =
            std::get_if<binder::BoundNullTestExpression>(
                &expression.node)) {
        if (!null_test->operand) {
            throw std::logic_error{
                "null test expression requires an operand"};
        }
        const auto operand = evaluate_expression(input, *null_test->operand);
        if (!operand) {
            return std::unexpected(operand.error());
        }
        const bool is_null = std::holds_alternative<types::NullValue>(
            require_value(*operand));
        return ExpressionResult{
            null_test->negated ? !is_null : is_null};
    }

    if (const auto* negation =
            std::get_if<binder::BoundNegationExpression>(
                &expression.node)) {
        if (!negation->operand) {
            throw std::logic_error{
                "negation expression requires an operand"};
        }
        const auto operand =
            evaluate_expression(input, *negation->operand);
        if (!operand) {
            return std::unexpected(operand.error());
        }
        return ExpressionResult{!require_boolean(*operand)};
    }

    if (const auto* unary =
            std::get_if<binder::BoundUnaryArithmeticExpression>(
                &expression.node)) {
        if (!unary->operand) {
            throw std::logic_error{
                "unary arithmetic expression requires an operand"};
        }
        const auto operand = evaluate_expression(input, *unary->operand);
        if (!operand) {
            return std::unexpected(operand.error());
        }
        auto result = evaluate_unary_arithmetic(
            require_value(*operand), unary->operation, unary->location);
        if (!result) {
            return std::unexpected(result.error());
        }
        return ExpressionResult{std::move(*result)};
    }

    if (const auto* binary =
            std::get_if<binder::BoundBinaryArithmeticExpression>(
                &expression.node)) {
        if (!binary->left || !binary->right) {
            throw std::logic_error{
                "binary arithmetic expression requires two operands"};
        }
        const auto left = evaluate_expression(input, *binary->left);
        if (!left) {
            return std::unexpected(left.error());
        }
        const auto right = evaluate_expression(input, *binary->right);
        if (!right) {
            return std::unexpected(right.error());
        }
        auto result = evaluate_binary_arithmetic(
            require_value(*left), require_value(*right),
            binary->operation, binary->location);
        if (!result) {
            return std::unexpected(result.error());
        }
        return ExpressionResult{std::move(*result)};
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

    const auto left_result = evaluate_expression(input, *logical->left);
    if (!left_result) {
        return std::unexpected(left_result.error());
    }
    const bool left = require_boolean(*left_result);
    switch (logical->operation) {
        case binder::BoundLogicalOperator::conjunction: {
            if (!left) {
                return ExpressionResult{false};
            }
            const auto right = evaluate_expression(input, *logical->right);
            if (!right) {
                return std::unexpected(right.error());
            }
            return ExpressionResult{require_boolean(*right)};
        }
        case binder::BoundLogicalOperator::disjunction: {
            if (left) {
                return ExpressionResult{true};
            }
            const auto right = evaluate_expression(input, *logical->right);
            if (!right) {
                return std::unexpected(right.error());
            }
            return ExpressionResult{require_boolean(*right)};
        }
    }
    throw std::logic_error{"unknown bound logical operator"};
}

}  // namespace nessodb::execution
