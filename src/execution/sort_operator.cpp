#include "execution/sort_operator.hpp"

#include "types/value.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace minidb::execution {
namespace {

int compare_values(const types::Value& left, const types::Value& right) {
    const bool left_is_null = std::holds_alternative<types::NullValue>(left);
    const bool right_is_null = std::holds_alternative<types::NullValue>(right);
    if (left_is_null || right_is_null) {
        if (left_is_null == right_is_null) {
            return 0;
        }
        return left_is_null ? 1 : -1;
    }
    if (left.index() != right.index()) {
        throw std::logic_error{"sort key and stored value types diverged"};
    }

    if (const auto* left_integer = std::get_if<std::int64_t>(&left)) {
        const auto right_integer = std::get<std::int64_t>(right);
        if (*left_integer < right_integer) {
            return -1;
        }
        return *left_integer > right_integer ? 1 : 0;
    }

    const auto& left_text = std::get<std::string>(left);
    const auto& right_text = std::get<std::string>(right);
    if (left_text < right_text) {
        return -1;
    }
    return left_text > right_text ? 1 : 0;
}

}  // namespace

SortOperator::SortOperator(std::unique_ptr<RowOperator> child,
                           std::vector<SortKey> keys,
                           std::size_t batch_size)
    : child_(std::move(child)),
      keys_(std::move(keys)),
      batch_size_(batch_size) {
    if (!child_) {
        throw std::invalid_argument{"sort operator requires a child"};
    }
    if (keys_.empty()) {
        throw std::invalid_argument{"sort operator requires at least one key"};
    }
    if (batch_size_ == 0) {
        throw std::invalid_argument{"row batch size must be greater than zero"};
    }
}

std::optional<RowBatch> SortOperator::next() {
    if (!materialized_) {
        materialize();
    }
    if (offset_ == rows_.size()) {
        return std::nullopt;
    }

    const std::size_t end = std::min(offset_ + batch_size_, rows_.size());
    RowBatch batch;
    batch.rows.reserve(end - offset_);
    while (offset_ < end) {
        batch.rows.push_back(std::move(rows_[offset_]));
        ++offset_;
    }
    return batch;
}

void SortOperator::materialize() {
    while (auto batch = child_->next()) {
        rows_.insert(rows_.end(),
                     std::make_move_iterator(batch->rows.begin()),
                     std::make_move_iterator(batch->rows.end()));
    }

    std::stable_sort(rows_.begin(), rows_.end(),
                     [this](const storage::Row& left,
                            const storage::Row& right) {
                         for (const auto& key : keys_) {
                             if (key.column_index >= left.values.size() ||
                                 key.column_index >= right.values.size()) {
                                 throw std::logic_error{
                                     "table schema and stored row state diverged"};
                             }
                             const int comparison = compare_values(
                                 left.values[key.column_index],
                                 right.values[key.column_index]);
                             if (comparison == 0) {
                                 continue;
                             }
                             return key.direction == SortDirection::ascending
                                        ? comparison < 0
                                        : comparison > 0;
                         }
                         return false;
                     });
    materialized_ = true;
}

}  // namespace minidb::execution
