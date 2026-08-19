#include "execution/expression_evaluator.hpp"

#include <stdexcept>
#include <variant>

namespace minidb::execution {

types::Value evaluate_expression(
    std::span<const types::Value> input,
    const binder::BoundExpression& expression) {
    if (const auto* literal =
            std::get_if<binder::BoundLiteralExpression>(&expression.node)) {
        return literal->value;
    }

    const auto* column =
        std::get_if<binder::BoundColumnReferenceExpression>(
            &expression.node);
    if (column == nullptr) {
        throw std::logic_error{
            "bound expression does not produce a stored value"};
    }
    const auto column_index = column->column_index;
    if (column_index >= input.size()) {
        throw std::logic_error{
            "table schema and stored row state diverged"};
    }
    return input[column_index];
}

}  // namespace minidb::execution
