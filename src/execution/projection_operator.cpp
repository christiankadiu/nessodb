#include "execution/projection_operator.hpp"

#include <stdexcept>
#include <utility>
#include <variant>

namespace minidb::execution {

ProjectionOperator::ProjectionOperator(
    std::unique_ptr<RowOperator> child,
    const std::vector<binder::BoundSelectExpression>& expressions)
    : child_(std::move(child)), expressions_(expressions) {
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
            if (const auto* literal =
                    std::get_if<binder::BoundLiteralExpression>(&expression)) {
                output_row.values.push_back(literal->value);
                continue;
            }

            const auto column_index =
                std::get<binder::BoundColumnReferenceExpression>(expression)
                    .column_index;
            if (column_index >= input_row.values.size()) {
                throw std::logic_error{
                    "table schema and stored row state diverged"};
            }
            output_row.values.push_back(input_row.values[column_index]);
        }
        output.rows.push_back(std::move(output_row));
    }
    return std::optional<RowBatch>{std::move(output)};
}

}  // namespace minidb::execution
