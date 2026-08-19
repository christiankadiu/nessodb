#include "execution/filter_operator.hpp"

#include "execution/predicate_evaluator.hpp"

#include <stdexcept>
#include <utility>

namespace minidb::execution {

FilterOperator::FilterOperator(std::unique_ptr<RowOperator> child,
                               binder::BoundExpression predicate)
    : child_(std::move(child)), predicate_(std::move(predicate)) {
    if (!child_) {
        throw std::invalid_argument{"filter operator requires a child"};
    }
}

OperatorResult FilterOperator::next() {
    while (true) {
        auto input = child_->next();
        if (!input) {
            return std::unexpected(input.error());
        }
        if (!*input) {
            return std::optional<RowBatch>{};
        }

        RowBatch output;
        output.rows.reserve((*input)->rows.size());
        for (auto& row : (*input)->rows) {
            if (matches_predicate(row.values, predicate_)) {
                output.rows.push_back(std::move(row));
            }
        }
        if (!output.rows.empty()) {
            return std::optional<RowBatch>{std::move(output)};
        }
    }
}

}  // namespace minidb::execution
