#include "execution/projection_operator.hpp"

#include "execution/expression_evaluator.hpp"

#include <stdexcept>
#include <utility>

namespace nessodb::execution {

ProjectionOperator::ProjectionOperator(
    std::unique_ptr<RowOperator> child,
    std::vector<binder::BoundExpression> expressions)
    : child_(std::move(child)), expressions_(std::move(expressions)) {
    if (!child_) {
        throw std::invalid_argument{"projection operator requires a child"};
    }
    if (expressions_.empty()) {
        throw std::invalid_argument{
            "projection operator requires at least one expression"};
    }
    for (const auto& expression : expressions_) {
        if (expression.result_type() !=
            binder::BoundExpressionResultType::value) {
            throw std::invalid_argument{
                "projection operator requires value expressions"};
        }
    }
}

OperatorResult ProjectionOperator::next() {
    auto input = child_->next();
    if (!input) {
        return std::unexpected(input.error());
    }
    if (!*input) {
        return std::optional<RowBatch>{};
    }

    RowBatch output;
    output.rows.reserve((*input)->rows.size());
    for (const auto& input_row : (*input)->rows) {
        storage::Row output_row;
        output_row.values.reserve(expressions_.size());
        for (const auto& expression : expressions_) {
            auto result = evaluate_expression(input_row.values, expression);
            if (!result) {
                const auto code =
                    result.error().code ==
                            ExpressionErrorCode::integer_overflow
                        ? OperatorErrorCode::integer_overflow
                        : OperatorErrorCode::division_by_zero;
                return std::unexpected(
                    OperatorError{code, result.error().location});
            }
            auto* value = std::get_if<types::Value>(&*result);
            if (value == nullptr) {
                throw std::logic_error{
                    "projection expression does not produce a stored value"};
            }
            output_row.values.push_back(std::move(*value));
        }
        output.rows.push_back(std::move(output_row));
    }
    return std::optional<RowBatch>{std::move(output)};
}

}  // namespace nessodb::execution
