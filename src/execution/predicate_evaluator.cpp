#include "execution/predicate_evaluator.hpp"

#include "execution/expression_evaluator.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>

namespace minidb::execution {
namespace {

bool matches_comparison(const types::Value& left, const types::Value& right,
                        binder::BoundComparisonOperator comparison) {
    if (std::holds_alternative<types::NullValue>(left) ||
        std::holds_alternative<types::NullValue>(right)) {
        return false;
    }
    if (left.index() != right.index()) {
        throw std::logic_error{"bound predicate and stored value types diverged"};
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
    return false;
}

}  // namespace

bool matches_predicate(std::span<const types::Value> values,
                       const binder::BoundExpression& predicate) {
    if (const auto* comparison =
            std::get_if<binder::BoundComparisonExpression>(
                &predicate.node)) {
        if (!comparison->left || !comparison->right) {
            throw std::logic_error{
                "comparison expression requires two operands"};
        }
        const auto left = evaluate_expression(values, *comparison->left);
        const auto right = evaluate_expression(values, *comparison->right);
        return matches_comparison(left, right,
                                  comparison->comparison);
    }

    if (const auto* null_predicate =
            std::get_if<binder::BoundNullTestExpression>(
                &predicate.node)) {
        if (!null_predicate->operand) {
            throw std::logic_error{
                "null test expression requires an operand"};
        }
        const bool is_null = std::holds_alternative<types::NullValue>(
            evaluate_expression(values, *null_predicate->operand));
        return null_predicate->negated ? !is_null : is_null;
    }

    if (const auto* negation =
            std::get_if<binder::BoundNegationExpression>(
                &predicate.node)) {
        if (!negation->operand) {
            throw std::logic_error{
                "negation expression requires an operand"};
        }
        return !matches_predicate(values, *negation->operand);
    }

    const auto* logical = std::get_if<binder::BoundLogicalExpression>(
        &predicate.node);
    if (logical == nullptr) {
        throw std::logic_error{
            "bound expression does not produce a predicate"};
    }
    if (!logical->left || !logical->right) {
        throw std::logic_error{
            "logical expression requires two operands"};
    }
    switch (logical->operation) {
        case binder::BoundLogicalOperator::conjunction:
            return matches_predicate(values, *logical->left) &&
                   matches_predicate(values, *logical->right);
        case binder::BoundLogicalOperator::disjunction:
            return matches_predicate(values, *logical->left) ||
                   matches_predicate(values, *logical->right);
    }
    return false;
}

}  // namespace minidb::execution
