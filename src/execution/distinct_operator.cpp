#include "execution/distinct_operator.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace nessodb::execution {
namespace {

void combine_hash(std::size_t& seed, std::size_t value) noexcept {
    seed ^= value + std::size_t{0x9e3779b9U} + (seed << 6U) + (seed >> 2U);
}

std::size_t estimated_key_size(
    const std::vector<types::Value>& values) noexcept {
    std::size_t size = sizeof(values) + 2 * sizeof(void*);
    const auto add = [&size](std::size_t amount) {
        if (amount > std::numeric_limits<std::size_t>::max() - size) {
            size = std::numeric_limits<std::size_t>::max();
        } else {
            size += amount;
        }
    };
    if (values.size() >
        std::numeric_limits<std::size_t>::max() / sizeof(types::Value)) {
        return std::numeric_limits<std::size_t>::max();
    }
    add(values.size() * sizeof(types::Value));
    for (const auto& value : values) {
        if (const auto* text = std::get_if<std::string>(&value)) {
            add(text->size());
        }
    }
    return size;
}

}  // namespace

std::size_t DistinctOperator::RowHash::operator()(
    const std::vector<types::Value>& values) const noexcept {
    std::size_t seed = values.size();
    for (const auto& value : values) {
        combine_hash(seed, value.index());
        if (const auto* integer = std::get_if<std::int64_t>(&value)) {
            combine_hash(seed, std::hash<std::int64_t>{}(*integer));
        } else if (const auto* text = std::get_if<std::string>(&value)) {
            combine_hash(seed, std::hash<std::string>{}(*text));
        }
    }
    return seed;
}

DistinctOperator::DistinctOperator(
    std::unique_ptr<RowOperator> child,
    std::size_t batch_size,
    std::size_t memory_limit)
    : child_(std::move(child)),
      batch_size_(batch_size),
      memory_limit_(memory_limit) {
    if (!child_) {
        throw std::invalid_argument{"distinct operator requires a child"};
    }
    if (batch_size_ == 0) {
        throw std::invalid_argument{"row batch size must be greater than zero"};
    }
}

OperatorResult DistinctOperator::next() {
    RowBatch output;
    output.rows.reserve(batch_size_);

    while (output.rows.size() < batch_size_) {
        if (!input_batch_ || input_offset_ == input_batch_->rows.size()) {
            auto input = child_->next();
            if (!input) {
                return std::unexpected(input.error());
            }
            if (!*input) {
                if (output.rows.empty()) {
                    return std::optional<RowBatch>{};
                }
                return std::optional<RowBatch>{std::move(output)};
            }
            input_batch_ = std::move(**input);
            input_offset_ = 0;
            if (input_batch_->rows.empty()) {
                continue;
            }
        }

        auto& row = input_batch_->rows[input_offset_];
        ++input_offset_;
        if (seen_.contains(row.values)) {
            continue;
        }

        const std::size_t key_size = estimated_key_size(row.values);
        if (key_size > memory_limit_ ||
            memory_used_ > memory_limit_ - key_size) {
            return std::unexpected(
                OperatorError{OperatorErrorCode::memory_limit_exceeded});
        }
        memory_used_ += key_size;
        seen_.insert(row.values);
        output.rows.push_back(std::move(row));
    }

    return std::optional<RowBatch>{std::move(output)};
}

}  // namespace nessodb::execution
