#include "execution/limit_operator.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace minidb::execution {

LimitOperator::LimitOperator(
    std::unique_ptr<RowOperator> child,
    std::size_t limit,
    std::size_t offset)
    : child_(std::move(child)), limit_(limit), offset_(offset) {
    if (!child_) {
        throw std::invalid_argument{"limit operator requires a child"};
    }
}

OperatorResult LimitOperator::next() {
    while (emitted_ < limit_) {
        auto input = child_->next();
        if (!input) {
            return std::unexpected(input.error());
        }
        if (!*input) {
            return std::optional<RowBatch>{};
        }

        auto& rows = (*input)->rows;
        const auto rows_to_skip =
            std::min(offset_ - skipped_, rows.size());
        skipped_ += rows_to_skip;
        if (rows_to_skip == rows.size()) {
            continue;
        }

        const auto rows_to_emit = std::min(
            limit_ - emitted_, rows.size() - rows_to_skip);
        RowBatch output;
        output.rows.reserve(rows_to_emit);
        const auto first = rows.begin() +
                           static_cast<std::ptrdiff_t>(rows_to_skip);
        const auto last = first +
                          static_cast<std::ptrdiff_t>(rows_to_emit);
        output.rows.insert(
            output.rows.end(),
            std::make_move_iterator(first),
            std::make_move_iterator(last));
        emitted_ += rows_to_emit;
        return std::optional<RowBatch>{std::move(output)};
    }

    return std::optional<RowBatch>{};
}

}  // namespace minidb::execution
