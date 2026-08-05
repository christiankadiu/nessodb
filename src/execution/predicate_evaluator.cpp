#include "execution/predicate_evaluator.hpp"

#include <cstdint>
#include <memory>
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
                       const binder::BoundPredicate& predicate) {
    if (const auto* comparison =
            std::get_if<binder::BoundComparisonPredicate>(&predicate)) {
        if (comparison->column_index >= values.size()) {
            throw std::logic_error{"table schema and stored row state diverged"};
        }
        return matches_comparison(values[comparison->column_index],
                                  comparison->value,
                                  comparison->comparison);
    }

    if (const auto* null_predicate =
            std::get_if<binder::BoundNullPredicate>(&predicate)) {
        if (null_predicate->column_index >= values.size()) {
            throw std::logic_error{"table schema and stored row state diverged"};
        }
        const bool is_null = std::holds_alternative<types::NullValue>(
            values[null_predicate->column_index]);
        return null_predicate->negated ? !is_null : is_null;
    }

    if (const auto* negation =
            std::get_if<std::unique_ptr<binder::BoundNegationPredicate>>(
                &predicate)) {
        return !matches_predicate(values, (*negation)->operand);
    }

    const auto& logical =
        *std::get<std::unique_ptr<binder::BoundLogicalPredicate>>(predicate);
    switch (logical.operation) {
        case binder::BoundLogicalOperator::conjunction:
            return matches_predicate(values, logical.left) &&
                   matches_predicate(values, logical.right);
        case binder::BoundLogicalOperator::disjunction:
            return matches_predicate(values, logical.left) ||
                   matches_predicate(values, logical.right);
    }
    return false;
}

}  // namespace minidb::execution
