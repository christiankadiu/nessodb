#include "execution/projection_operator.hpp"

#include "execution/expression_evaluator.hpp"

#include <stdexcept>
#include <utility>

namespace minidb::execution {

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
            output_row.values.push_back(
                evaluate_expression(input_row.values, expression));
        }
        output.rows.push_back(std::move(output_row));
    }
    return std::optional<RowBatch>{std::move(output)};
}

}  // namespace minidb::execution
