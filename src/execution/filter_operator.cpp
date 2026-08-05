#include "execution/filter_operator.hpp"

#include "execution/predicate_evaluator.hpp"

#include <stdexcept>
#include <utility>

namespace minidb::execution {

FilterOperator::FilterOperator(std::unique_ptr<RowOperator> child,
                               const binder::BoundPredicate& predicate)
    : child_(std::move(child)), predicate_(predicate) {
    if (!child_) {
        throw std::invalid_argument{"filter operator requires a child"};
    }
}

std::optional<RowBatch> FilterOperator::next() {
    while (auto input = child_->next()) {
        RowBatch output;
        output.rows.reserve(input->rows.size());
        for (auto& row : input->rows) {
            if (matches_predicate(row.values, predicate_)) {
                output.rows.push_back(std::move(row));
            }
        }
        if (!output.rows.empty()) {
            return output;
        }
    }
    return std::nullopt;
}

}  // namespace minidb::execution
