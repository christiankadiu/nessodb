#include "execution/hash_aggregate_operator.hpp"

#include "execution/expression_evaluator.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace minidb::execution {
namespace {

void combine_hash(std::size_t& seed, std::size_t value) noexcept {
    seed ^= value + std::size_t{0x9e3779b9U} + (seed << 6U) + (seed >> 2U);
}

void saturating_add(std::size_t& target, std::size_t amount) noexcept {
    if (amount > std::numeric_limits<std::size_t>::max() - target) {
        target = std::numeric_limits<std::size_t>::max();
    } else {
        target += amount;
    }
}

std::size_t estimated_key_size(
    const std::vector<types::Value>& values) noexcept {
    std::size_t size = sizeof(values) + 2 * sizeof(void*);
    if (values.size() >
        std::numeric_limits<std::size_t>::max() / sizeof(types::Value)) {
        return std::numeric_limits<std::size_t>::max();
    }
    saturating_add(size, values.size() * sizeof(types::Value));
    for (const auto& value : values) {
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

OperatorError aggregate_error(const AggregateError& error,
                              sql::SourceLocation location) {
    if (error.code == AggregateErrorCode::invalid_argument_count) {
        throw std::logic_error{
            "aggregate state rejected its declared arity"};
    }
    const auto code = error.code == AggregateErrorCode::integer_overflow
                          ? OperatorErrorCode::integer_overflow
                          : OperatorErrorCode::type_mismatch;
    return OperatorError{code, location};
}

}  // namespace

std::size_t HashAggregateOperator::ValueVectorHash::operator()(
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

HashAggregateOperator::HashAggregateOperator(
    std::unique_ptr<RowOperator> child,
    std::vector<binder::BoundExpression> group_keys,
    std::vector<AggregateComputation> aggregates,
    std::size_t batch_size,
    std::size_t memory_limit)
    : child_(std::move(child)),
      group_keys_(std::move(group_keys)),
      aggregates_(std::move(aggregates)),
      batch_size_(batch_size),
      memory_limit_(memory_limit) {
    if (!child_) {
        throw std::invalid_argument{
            "hash aggregate operator requires a child"};
    }
    if (group_keys_.empty()) {
        throw std::invalid_argument{
            "hash aggregate operator requires at least one group key"};
    }
    if (batch_size_ == 0) {
        throw std::invalid_argument{"row batch size must be greater than zero"};
    }
    for (const auto& key : group_keys_) {
        if (key.result_type() !=
            binder::BoundExpressionResultType::value) {
            throw std::invalid_argument{
                "group keys must be value expressions"};
        }
    }
    for (const auto& aggregate : aggregates_) {
        if (!aggregate.state) {
            throw std::invalid_argument{
                "aggregate computation requires a state"};
        }
        if (aggregate.arguments.size() !=
            aggregate.state->argument_count()) {
            throw std::invalid_argument{
                "aggregate argument count does not match its state"};
        }
        for (const auto& argument : aggregate.arguments) {
            if (argument.result_type() !=
                binder::BoundExpressionResultType::value) {
                throw std::invalid_argument{
                    "aggregate arguments must be value expressions"};
            }
        }
    }
}

OperatorResult HashAggregateOperator::next() {
    if (!materialized_) {
        auto result = materialize();
        if (!result) {
            return std::unexpected(result.error());
        }
    }
    if (output_offset_ == groups_.size()) {
        return std::optional<RowBatch>{};
    }

    const auto end = output_offset_ +
                     std::min(batch_size_, groups_.size() - output_offset_);
    RowBatch output;
    output.rows.reserve(end - output_offset_);
    while (output_offset_ < end) {
        auto& group = groups_[output_offset_];
        storage::Row row;
        row.values = std::move(group.keys);
        row.values.reserve(row.values.size() + group.states.size());
        for (const auto& state : group.states) {
            row.values.push_back(state->finalize());
        }
        output.rows.push_back(std::move(row));
        ++output_offset_;
    }
    return std::optional<RowBatch>{std::move(output)};
}

std::expected<void, OperatorError> HashAggregateOperator::materialize() {
    while (true) {
        auto input = child_->next();
        if (!input) {
            return std::unexpected(input.error());
        }
        if (!*input) {
            break;
        }

        for (const auto& row : (*input)->rows) {
            std::vector<types::Value> keys;
            keys.reserve(group_keys_.size());
            for (const auto& expression : group_keys_) {
                auto evaluated = evaluate_expression(row.values, expression);
                if (!evaluated) {
                    return std::unexpected(
                        expression_error(evaluated.error()));
                }
                auto* value = std::get_if<types::Value>(&*evaluated);
                if (value == nullptr) {
                    throw std::logic_error{
                        "group key did not produce a value"};
                }
                keys.push_back(std::move(*value));
            }

            auto found = group_indexes_.find(keys);
            std::size_t group_index;
            if (found == group_indexes_.end()) {
                std::size_t group_size = sizeof(Group) + sizeof(std::size_t);
                const auto key_size = estimated_key_size(keys);
                saturating_add(group_size, key_size);
                saturating_add(group_size, key_size);
                if (aggregates_.size() >
                    std::numeric_limits<std::size_t>::max() /
                        sizeof(std::unique_ptr<AggregateState>)) {
                    group_size = std::numeric_limits<std::size_t>::max();
                } else {
                    saturating_add(
                        group_size,
                        aggregates_.size() *
                            sizeof(std::unique_ptr<AggregateState>));
                }
                Group group;
                group.keys = keys;
                group.states.reserve(aggregates_.size());
                for (const auto& aggregate : aggregates_) {
                    auto state = aggregate.state->clone_empty();
                    saturating_add(group_size,
                                   state->estimated_memory_usage());
                    group.states.push_back(std::move(state));
                }
                if (group_size > memory_limit_ ||
                    memory_used_ > memory_limit_ - group_size) {
                    return std::unexpected(OperatorError{
                        OperatorErrorCode::memory_limit_exceeded});
                }
                memory_used_ += group_size;
                group_index = groups_.size();
                group_indexes_.emplace(keys, group_index);
                groups_.push_back(std::move(group));
            } else {
                group_index = found->second;
            }

            auto& group = groups_[group_index];
            for (std::size_t index = 0; index < aggregates_.size(); ++index) {
                const auto& aggregate = aggregates_[index];
                auto& state = group.states[index];
                std::vector<types::Value> arguments;
                arguments.reserve(aggregate.arguments.size());
                for (const auto& expression : aggregate.arguments) {
                    auto evaluated =
                        evaluate_expression(row.values, expression);
                    if (!evaluated) {
                        return std::unexpected(
                            expression_error(evaluated.error()));
                    }
                    auto* value = std::get_if<types::Value>(&*evaluated);
                    if (value == nullptr) {
                        throw std::logic_error{
                            "aggregate argument did not produce a value"};
                    }
                    arguments.push_back(std::move(*value));
                }
                const auto previous_size = state->estimated_memory_usage();
                auto accumulated = state->accumulate(arguments);
                if (!accumulated) {
                    return std::unexpected(aggregate_error(
                        accumulated.error(), aggregate.location));
                }
                const auto current_size = state->estimated_memory_usage();
                if (current_size > previous_size) {
                    const auto growth = current_size - previous_size;
                    if (growth > memory_limit_ ||
                        memory_used_ > memory_limit_ - growth) {
                        return std::unexpected(OperatorError{
                            OperatorErrorCode::memory_limit_exceeded});
                    }
                    memory_used_ += growth;
                }
            }
        }
    }
    materialized_ = true;
    return {};
}

}  // namespace minidb::execution
