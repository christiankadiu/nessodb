#include "execution/aggregate_state.hpp"

#include <limits>
#include <variant>

namespace minidb::execution {

CountAggregateState::CountAggregateState(CountMode mode) noexcept
    : mode_(mode) {}

std::size_t CountAggregateState::argument_count() const noexcept {
    return mode_ == CountMode::all_rows ? 0 : 1;
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

}  // namespace minidb::execution
