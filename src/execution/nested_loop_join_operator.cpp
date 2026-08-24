#include "execution/nested_loop_join_operator.hpp"

#include "execution/expression_evaluator.hpp"

#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace minidb::execution {
namespace {

void saturating_add(std::size_t& target, std::size_t amount) noexcept {
    if (amount > std::numeric_limits<std::size_t>::max() - target) {
        target = std::numeric_limits<std::size_t>::max();
    } else {
        target += amount;
    }
}

std::size_t estimated_row_size(const storage::Row& row) noexcept {
    std::size_t size = sizeof(storage::Row);
    if (row.values.size() >
        std::numeric_limits<std::size_t>::max() / sizeof(types::Value)) {
        return std::numeric_limits<std::size_t>::max();
    }
    saturating_add(size, row.values.size() * sizeof(types::Value));
    for (const auto& value : row.values) {
        if (const auto* text = std::get_if<std::string>(&value)) {
            saturating_add(size, text->size());
        }
    }
    return size;
}

OperatorError expression_error(const ExpressionError& error) noexcept {
    const auto code =
        error.code == ExpressionErrorCode::integer_overflow
            ? OperatorErrorCode::integer_overflow
            : OperatorErrorCode::division_by_zero;
    return OperatorError{code, error.location};
}

}  // namespace

NestedLoopJoinOperator::NestedLoopJoinOperator(
    std::unique_ptr<RowOperator> left,
    std::unique_ptr<RowOperator> right,
    std::optional<binder::BoundExpression> predicate,
    std::size_t batch_size,
    std::size_t memory_limit)
    : left_(std::move(left)),
      right_(std::move(right)),
      predicate_(std::move(predicate)),
      batch_size_(batch_size),
      memory_limit_(memory_limit) {
    if (!left_ || !right_) {
        throw std::invalid_argument{
            "nested loop join operator requires two children"};
    }
    if (predicate_ && predicate_->result_type() !=
                          binder::BoundExpressionResultType::boolean) {
        throw std::invalid_argument{
            "join predicate must be a boolean expression"};
    }
    if (batch_size_ == 0) {
        throw std::invalid_argument{"row batch size must be greater than zero"};
    }
}

OperatorResult NestedLoopJoinOperator::next() {
    if (!right_materialized_) {
        auto materialized = materialize_right();
        if (!materialized) {
            return std::unexpected(materialized.error());
        }
    }
    if (right_rows_.empty()) {
        return std::optional<RowBatch>{};
    }

    RowBatch output;
    output.rows.reserve(batch_size_);
    while (output.rows.size() < batch_size_) {
        if (!left_batch_ || left_offset_ == left_batch_->rows.size()) {
            auto input = left_->next();
            if (!input) {
                return std::unexpected(input.error());
            }
            if (!*input) {
                if (output.rows.empty()) {
                    return std::optional<RowBatch>{};
                }
                return std::optional<RowBatch>{std::move(output)};
            }
            left_batch_ = std::move(**input);
            left_offset_ = 0;
            right_offset_ = 0;
            if (left_batch_->rows.empty()) {
                continue;
            }
        }

        const auto& left_row = left_batch_->rows[left_offset_];
        const auto& right_row = right_rows_[right_offset_];
        storage::Row joined;
        joined.values.reserve(left_row.values.size() +
                              right_row.values.size());
        joined.values.insert(joined.values.end(), left_row.values.begin(),
                             left_row.values.end());
        joined.values.insert(joined.values.end(), right_row.values.begin(),
                             right_row.values.end());

        ++right_offset_;
        if (right_offset_ == right_rows_.size()) {
            right_offset_ = 0;
            ++left_offset_;
        }

        bool matches = true;
        if (predicate_) {
            auto evaluated = evaluate_expression(joined.values, *predicate_);
            if (!evaluated) {
                return std::unexpected(
                    expression_error(evaluated.error()));
            }
            const auto* value = std::get_if<bool>(&*evaluated);
            if (value == nullptr) {
                throw std::logic_error{
                    "join predicate did not produce a boolean"};
            }
            matches = *value;
        }
        if (matches) {
            output.rows.push_back(std::move(joined));
        }
    }
    return std::optional<RowBatch>{std::move(output)};
}

std::expected<void, OperatorError>
NestedLoopJoinOperator::materialize_right() {
    std::size_t memory_used = 0;
    while (true) {
        auto input = right_->next();
        if (!input) {
            return std::unexpected(input.error());
        }
        if (!*input) {
            break;
        }
        for (const auto& row : (*input)->rows) {
            const auto row_size = estimated_row_size(row);
            if (row_size > memory_limit_ ||
                memory_used > memory_limit_ - row_size) {
                return std::unexpected(OperatorError{
                    OperatorErrorCode::memory_limit_exceeded});
            }
            memory_used += row_size;
        }
        right_rows_.insert(
            right_rows_.end(),
            std::make_move_iterator((*input)->rows.begin()),
            std::make_move_iterator((*input)->rows.end()));
    }
    right_materialized_ = true;
    return {};
}

}  // namespace minidb::execution
