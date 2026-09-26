#include "execution/expression_evaluator.hpp"

#include "binder/bound_statement.hpp"
#include "types/value.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

bool evaluate_boolean(
    std::span<const nessodb::types::Value> values,
    const nessodb::binder::BoundExpression& expression) {
    const auto result =
        nessodb::execution::evaluate_expression(values, expression);
    return result && std::get<bool>(*result);
}

nessodb::binder::BoundExpression column_expression(std::size_t index) {
    return nessodb::binder::BoundExpression{
        nessodb::binder::BoundColumnReferenceExpression{index, {}}};
}

nessodb::binder::BoundExpression literal_expression(
    nessodb::types::Value value) {
    return nessodb::binder::BoundExpression{
        nessodb::binder::BoundLiteralExpression{
            std::move(value), {}}};
}

nessodb::binder::BoundExpression comparison_expression(
    std::size_t column_index,
    nessodb::binder::BoundComparisonOperator comparison,
    nessodb::types::Value value) {
    return nessodb::binder::BoundExpression{
        nessodb::binder::BoundComparisonExpression{
            comparison,
            std::make_unique<nessodb::binder::BoundExpression>(
                column_expression(column_index)),
            std::make_unique<nessodb::binder::BoundExpression>(
                literal_expression(std::move(value)))}};
}

nessodb::binder::BoundExpression null_test_expression(
    std::size_t column_index, bool negated) {
    return nessodb::binder::BoundExpression{
        nessodb::binder::BoundNullTestExpression{
            std::make_unique<nessodb::binder::BoundExpression>(
                column_expression(column_index)),
            negated}};
}

void test_comparisons() {
    const std::vector<nessodb::types::Value> values{
        std::int64_t{2}, std::string{"Alice"}, nessodb::types::NullValue{}};

    using nessodb::binder::BoundComparisonOperator;
    const auto equality = comparison_expression(
        0, BoundComparisonOperator::equal,
        nessodb::types::Value{std::int64_t{2}});
    expect(equality.result_type() ==
               nessodb::binder::BoundExpressionResultType::boolean,
           "comparison expression declares a boolean result");
    expect(evaluate_boolean(
               values, equality),
           "integer equality matches");
    expect(evaluate_boolean(
               values, comparison_expression(
                           1, BoundComparisonOperator::less,
                           nessodb::types::Value{std::string{"Bob"}})),
           "text ordering matches");
    expect(!evaluate_boolean(
               values, comparison_expression(
                           2, BoundComparisonOperator::equal,
                           nessodb::types::Value{
                               nessodb::types::NullValue{}})),
           "NULL comparison does not match");
}

void test_null_and_logical_predicates() {
    const std::vector<nessodb::types::Value> values{
        std::int64_t{2}, nessodb::types::NullValue{}};

    using nessodb::binder::BoundComparisonOperator;
    using nessodb::binder::BoundExpression;
    using nessodb::binder::BoundLogicalExpression;
    using nessodb::binder::BoundLogicalOperator;
    using nessodb::binder::BoundNegationExpression;
    expect(evaluate_boolean(values, null_test_expression(1, false)),
           "IS NULL matches a NULL value");
    expect(!evaluate_boolean(values, null_test_expression(1, true)),
           "IS NOT NULL rejects a NULL value");

    BoundExpression conjunction{BoundLogicalExpression{
        BoundLogicalOperator::conjunction,
        std::make_unique<BoundExpression>(comparison_expression(
            0, BoundComparisonOperator::greater,
            nessodb::types::Value{std::int64_t{1}})),
        std::make_unique<BoundExpression>(
            null_test_expression(1, false))}};
    expect(evaluate_boolean(values, conjunction),
           "logical conjunction combines predicate results");

    BoundExpression negation{BoundNegationExpression{
        std::make_unique<BoundExpression>(
            null_test_expression(1, false))}};
    expect(!evaluate_boolean(values, negation),
           "logical negation inverts its operand");
}

}  // namespace

int main() {
    test_comparisons();
    test_null_and_logical_predicates();

    if (failures != 0) {
        std::cerr << failures << " predicate evaluator assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
