#include "execution/aggregate_state.hpp"

#include <limits>
#include <variant>

namespace minidb::execution {

CountAggregateState::CountAggregateState(CountMode mode) noexcept
    : mode_(mode) {}

std::size_t CountAggregateState::argument_count() const noexcept {
    return mode_ == CountMode::all_rows ? 0 : 1;
}

std::unique_ptr<AggregateState> CountAggregateState::clone_empty() const {
    return std::make_unique<CountAggregateState>(mode_);
}

std::size_t CountAggregateState::estimated_memory_usage() const noexcept {
    return sizeof(*this);
}

AggregateResult CountAggregateState::accumulate(
    std::span<const types::Value> arguments) {
    if (arguments.size() != argument_count()) {
        return std::unexpected(AggregateError{
            AggregateErrorCode::invalid_argument_count});
    }
    if (mode_ == CountMode::non_null_values &&
        std::holds_alternative<types::NullValue>(arguments.front())) {
        return {};
    }
    if (count_ == std::numeric_limits<std::int64_t>::max()) {
        return std::unexpected(AggregateError{
            AggregateErrorCode::integer_overflow});
    }
    ++count_;
    return {};
}

types::Value CountAggregateState::finalize() const {
    return types::Value{count_};
}

MinMaxAggregateState::MinMaxAggregateState(MinMaxMode mode) noexcept
    : mode_(mode) {}

std::size_t MinMaxAggregateState::argument_count() const noexcept {
    return 1;
}

std::unique_ptr<AggregateState> MinMaxAggregateState::clone_empty() const {
    return std::make_unique<MinMaxAggregateState>(mode_);
}

std::size_t MinMaxAggregateState::estimated_memory_usage() const noexcept {
    const auto* text = value_ ? std::get_if<std::string>(&*value_) : nullptr;
    return sizeof(*this) + (text == nullptr ? 0 : text->size());
}

AggregateResult MinMaxAggregateState::accumulate(
    std::span<const types::Value> arguments) {
    if (arguments.size() != argument_count()) {
        return std::unexpected(AggregateError{
            AggregateErrorCode::invalid_argument_count});
    }
    const auto& candidate = arguments.front();
    if (std::holds_alternative<types::NullValue>(candidate)) {
        return {};
    }
    if (!std::holds_alternative<std::int64_t>(candidate) &&
        !std::holds_alternative<std::string>(candidate)) {
        return std::unexpected(AggregateError{
            AggregateErrorCode::type_mismatch});
    }
    if (!value_) {
        value_ = candidate;
        return {};
    }
    if (value_->index() != candidate.index()) {
        return std::unexpected(AggregateError{
            AggregateErrorCode::type_mismatch});
    }

    bool less = false;
    if (const auto* integer = std::get_if<std::int64_t>(&candidate)) {
        less = *integer < std::get<std::int64_t>(*value_);
    } else {
        less = std::get<std::string>(candidate) <
               std::get<std::string>(*value_);
    }
    if ((mode_ == MinMaxMode::minimum && less) ||
        (mode_ == MinMaxMode::maximum && !less && candidate != *value_)) {
        value_ = candidate;
    }
    return {};
}

types::Value MinMaxAggregateState::finalize() const {
    return value_.value_or(types::Value{types::NullValue{}});
}

std::size_t SumAggregateState::argument_count() const noexcept {
    return 1;
}

std::unique_ptr<AggregateState> SumAggregateState::clone_empty() const {
    return std::make_unique<SumAggregateState>();
}

std::size_t SumAggregateState::estimated_memory_usage() const noexcept {
    return sizeof(*this);
}

AggregateResult SumAggregateState::accumulate(
    std::span<const types::Value> arguments) {
    if (arguments.size() != argument_count()) {
        return std::unexpected(AggregateError{
            AggregateErrorCode::invalid_argument_count});
    }
    const auto& value = arguments.front();
    if (std::holds_alternative<types::NullValue>(value)) {
        return {};
    }
    const auto* integer = std::get_if<std::int64_t>(&value);
    if (integer == nullptr) {
        return std::unexpected(AggregateError{
            AggregateErrorCode::type_mismatch});
    }
    if (!sum_) {
        sum_ = *integer;
        return {};
    }
    if ((*integer > 0 &&
         *sum_ > std::numeric_limits<std::int64_t>::max() - *integer) ||
        (*integer < 0 &&
         *sum_ < std::numeric_limits<std::int64_t>::min() - *integer)) {
        return std::unexpected(AggregateError{
            AggregateErrorCode::integer_overflow});
    }
    *sum_ += *integer;
    return {};
}

types::Value SumAggregateState::finalize() const {
    if (!sum_) {
        return types::Value{types::NullValue{}};
    }
    return types::Value{*sum_};
}

}  // namespace minidb::execution
