#include "execution/expression_evaluator.hpp"

#include "binder/bound_expression.hpp"
#include "storage/access/row.hpp"
#include "types/value.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

nessodb::binder::BoundExpression integer_expression(std::int64_t value) {
    return nessodb::binder::BoundExpression{
        nessodb::binder::BoundLiteralExpression{
            nessodb::types::Value{value}, {}}};
}

nessodb::binder::BoundExpression null_expression() {
    return nessodb::binder::BoundExpression{
        nessodb::binder::BoundLiteralExpression{
            nessodb::types::Value{nessodb::types::NullValue{}}, {}}};
}

nessodb::binder::BoundExpression binary_expression(
    nessodb::binder::BoundBinaryArithmeticOperator operation,
    nessodb::binder::BoundExpression left,
    nessodb::binder::BoundExpression right,
    nessodb::sql::SourceLocation location = {}) {
    return nessodb::binder::BoundExpression{
        nessodb::binder::BoundBinaryArithmeticExpression{
            operation,
            std::make_unique<nessodb::binder::BoundExpression>(
                std::move(left)),
            std::make_unique<nessodb::binder::BoundExpression>(
                std::move(right)),
            location}};
}

nessodb::binder::BoundExpression unary_expression(
    nessodb::binder::BoundUnaryArithmeticOperator operation,
    nessodb::binder::BoundExpression operand,
    nessodb::sql::SourceLocation location = {}) {
    return nessodb::binder::BoundExpression{
        nessodb::binder::BoundUnaryArithmeticExpression{
            operation,
            std::make_unique<nessodb::binder::BoundExpression>(
                std::move(operand)),
            location}};
}

void test_literal_expression() {
    const nessodb::binder::BoundExpression expression{
        nessodb::binder::BoundLiteralExpression{
            nessodb::types::Value{std::int64_t{42}}, {}}};
    const auto result = nessodb::execution::evaluate_expression({}, expression);

    expect(expression.result_type() ==
               nessodb::binder::BoundExpressionResultType::value,
           "literal expression declares a value result");
    expect(result && std::get<std::int64_t>(
                         std::get<nessodb::types::Value>(*result)) == 42,
           "literal expression returns its bound value");
}

void test_column_expression() {
    const nessodb::storage::Row input{
        {std::int64_t{7}, std::string{"Alice"}}};
    const nessodb::binder::BoundExpression expression{
        nessodb::binder::BoundColumnReferenceExpression{1, {}}};
    const auto result = nessodb::execution::evaluate_expression(
        input.values, expression);

    expect(result && std::get<std::string>(
                         std::get<nessodb::types::Value>(*result)) == "Alice",
           "column expression reads its input position");
}

void test_invalid_column_expression() {
    const nessodb::storage::Row input{{std::int64_t{7}}};
    const nessodb::binder::BoundExpression expression{
        nessodb::binder::BoundColumnReferenceExpression{1, {}}};

    bool rejected = false;
    try {
        static_cast<void>(nessodb::execution::evaluate_expression(
            input.values, expression));
    } catch (const std::logic_error&) {
        rejected = true;
    }
    expect(rejected, "column expression rejects an invalid input position");
}

void test_arithmetic_expression() {
    const auto expression = binary_expression(
        nessodb::binder::BoundBinaryArithmeticOperator::addition,
        integer_expression(20), integer_expression(22));
    const auto result =
        nessodb::execution::evaluate_expression({}, expression);

    expect(result && std::get<std::int64_t>(
                         std::get<nessodb::types::Value>(*result)) == 42,
           "binary arithmetic returns its integer result");
}

void test_arithmetic_null_propagation() {
    const auto expression = binary_expression(
        nessodb::binder::BoundBinaryArithmeticOperator::multiplication,
        null_expression(), integer_expression(4));
    const auto result =
        nessodb::execution::evaluate_expression({}, expression);

    expect(result && std::holds_alternative<nessodb::types::NullValue>(
                         std::get<nessodb::types::Value>(*result)),
           "arithmetic propagates NULL operands");
}

void test_arithmetic_errors() {
    constexpr nessodb::sql::SourceLocation division_location{12, 1, 13};
    const auto division = binary_expression(
        nessodb::binder::BoundBinaryArithmeticOperator::division,
        integer_expression(10), integer_expression(0), division_location);
    const auto division_result =
        nessodb::execution::evaluate_expression({}, division);
    expect(!division_result &&
               division_result.error().code ==
                   nessodb::execution::ExpressionErrorCode::division_by_zero &&
               division_result.error().location == division_location,
           "division by zero reports its operator location");

    const auto overflow = binary_expression(
        nessodb::binder::BoundBinaryArithmeticOperator::addition,
        integer_expression(std::numeric_limits<std::int64_t>::max()),
        integer_expression(1));
    const auto overflow_result =
        nessodb::execution::evaluate_expression({}, overflow);
    expect(!overflow_result &&
               overflow_result.error().code ==
                   nessodb::execution::ExpressionErrorCode::integer_overflow,
           "integer overflow is reported without undefined behavior");

    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto negation = unary_expression(
        nessodb::binder::BoundUnaryArithmeticOperator::minus,
        integer_expression(minimum));
    const auto negation_result =
        nessodb::execution::evaluate_expression({}, negation);
    expect(!negation_result &&
               negation_result.error().code ==
                   nessodb::execution::ExpressionErrorCode::integer_overflow,
           "negating the minimum integer reports overflow");

    const auto multiplication = binary_expression(
        nessodb::binder::BoundBinaryArithmeticOperator::multiplication,
        integer_expression(minimum), integer_expression(-1));
    const auto multiplication_result =
        nessodb::execution::evaluate_expression({}, multiplication);
    expect(!multiplication_result &&
               multiplication_result.error().code ==
                   nessodb::execution::ExpressionErrorCode::integer_overflow,
           "multiplication overflow is reported safely");

    const auto division_overflow = binary_expression(
        nessodb::binder::BoundBinaryArithmeticOperator::division,
        integer_expression(minimum), integer_expression(-1));
    const auto division_overflow_result =
        nessodb::execution::evaluate_expression({}, division_overflow);
    expect(!division_overflow_result &&
               division_overflow_result.error().code ==
                   nessodb::execution::ExpressionErrorCode::integer_overflow,
           "division overflow is reported safely");
}

}  // namespace

int main() {
    test_literal_expression();
    test_column_expression();
    test_invalid_column_expression();
    test_arithmetic_expression();
    test_arithmetic_null_propagation();
    test_arithmetic_errors();

    if (failures != 0) {
        std::cerr << failures << " expression evaluator assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
