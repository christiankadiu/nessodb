#include "execution/sort_operator.hpp"

#include "types/value.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace nessodb::execution {
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

std::size_t estimated_row_size(const storage::Row& row) noexcept {
    std::size_t size = sizeof(storage::Row);
    const auto add = [&size](std::size_t amount) {
        if (amount > std::numeric_limits<std::size_t>::max() - size) {
            size = std::numeric_limits<std::size_t>::max();
        } else {
            size += amount;
        }
    };
    if (row.values.size() >
        std::numeric_limits<std::size_t>::max() / sizeof(types::Value)) {
        size = std::numeric_limits<std::size_t>::max();
    } else {
        add(row.values.size() * sizeof(types::Value));
    }
    for (const auto& value : row.values) {
        if (const auto* text = std::get_if<std::string>(&value)) {
            add(text->size());
        }
    }
    return size;
}

}  // namespace

SortOperator::SortOperator(std::unique_ptr<RowOperator> child,
                           std::vector<SortKey> keys,
                           std::size_t batch_size,
                           std::size_t memory_limit)
    : child_(std::move(child)),
      keys_(std::move(keys)),
      batch_size_(batch_size),
      memory_limit_(memory_limit) {
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

OperatorResult SortOperator::next() {
    if (!materialized_) {
        auto materialized = materialize();
        if (!materialized) {
            return std::unexpected(materialized.error());
        }
    }
    if (offset_ == rows_.size()) {
        return std::optional<RowBatch>{};
    }

    const std::size_t end =
        offset_ + std::min(batch_size_, rows_.size() - offset_);
    RowBatch batch;
    batch.rows.reserve(end - offset_);
    while (offset_ < end) {
        batch.rows.push_back(std::move(rows_[offset_]));
        ++offset_;
    }
    return std::optional<RowBatch>{std::move(batch)};
}

std::expected<void, OperatorError> SortOperator::materialize() {
    std::size_t memory_used = 0;
    while (true) {
        auto batch = child_->next();
        if (!batch) {
            return std::unexpected(batch.error());
        }
        if (!*batch) {
            break;
        }
        for (const auto& row : (*batch)->rows) {
            const std::size_t row_size = estimated_row_size(row);
            if (row_size > memory_limit_ ||
                memory_used > memory_limit_ - row_size) {
                return std::unexpected(
                    OperatorError{OperatorErrorCode::memory_limit_exceeded});
            }
            memory_used += row_size;
        }
        rows_.insert(rows_.end(),
                     std::make_move_iterator((*batch)->rows.begin()),
                     std::make_move_iterator((*batch)->rows.end()));
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
    return {};
}

}  // namespace nessodb::execution
