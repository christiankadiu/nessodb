#include "execution/aggregate_state.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
#include <variant>

namespace {

using nessodb::execution::AggregateErrorCode;
using nessodb::execution::CountAggregateState;
using nessodb::execution::CountMode;
using nessodb::execution::MinMaxAggregateState;
using nessodb::execution::MinMaxMode;
using nessodb::execution::SumAggregateState;
using nessodb::types::NullValue;
using nessodb::types::Value;

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

std::int64_t count_value(const CountAggregateState& state) {
    return std::get<std::int64_t>(state.finalize());
}

void test_count_all_rows() {
    CountAggregateState state{CountMode::all_rows};
    const std::span<const Value> no_arguments;

    expect(state.accumulate(no_arguments).has_value() &&
               state.accumulate(no_arguments).has_value(),
           "COUNT(*) accepts one step per input row");
    expect(count_value(state) == 2, "COUNT(*) counts every row");

    const std::array<Value, 1> unexpected{Value{std::int64_t{1}}};
    const auto invalid = state.accumulate(unexpected);
    expect(!invalid && invalid.error().code ==
                           AggregateErrorCode::invalid_argument_count,
           "COUNT(*) rejects value arguments");
}

void test_count_non_null_values() {
    CountAggregateState state{CountMode::non_null_values};
    const std::array<Value, 1> integer{Value{std::int64_t{7}}};
    const std::array<Value, 1> null{Value{NullValue{}}};

    expect(state.accumulate(integer).has_value() &&
               state.accumulate(null).has_value(),
           "COUNT(expression) accepts one value per row");
    expect(count_value(state) == 1,
           "COUNT(expression) ignores NULL values");

    const std::span<const Value> no_arguments;
    const auto invalid = state.accumulate(no_arguments);
    expect(!invalid && invalid.error().code ==
                           AggregateErrorCode::invalid_argument_count,
           "COUNT(expression) requires one argument");
}

void test_min_max_and_sum() {
    MinMaxAggregateState minimum{MinMaxMode::minimum};
    MinMaxAggregateState maximum{MinMaxMode::maximum};
    SumAggregateState sum;
    const std::array<Value, 1> seven{Value{std::int64_t{7}}};
    const std::array<Value, 1> minus_two{Value{std::int64_t{-2}}};
    const std::array<Value, 1> null{Value{NullValue{}}};

    for (const auto* value : {&seven, &null, &minus_two}) {
        expect(minimum.accumulate(*value).has_value() &&
                   maximum.accumulate(*value).has_value() &&
                   sum.accumulate(*value).has_value(),
               "numeric aggregates accept values and ignore NULL");
    }
    expect(std::get<std::int64_t>(minimum.finalize()) == -2 &&
               std::get<std::int64_t>(maximum.finalize()) == 7 &&
               std::get<std::int64_t>(sum.finalize()) == 5,
           "MIN, MAX, and SUM finalize their accumulated values");

    SumAggregateState empty_sum;
    expect(std::holds_alternative<NullValue>(empty_sum.finalize()),
           "SUM over no non-NULL values returns NULL");

    const std::array<Value, 1> text{Value{std::string{"text"}}};
    const auto mismatch = empty_sum.accumulate(text);
    expect(!mismatch && mismatch.error().code ==
                            AggregateErrorCode::type_mismatch,
           "SUM rejects non-integer values");
}

}  // namespace

int main() {
    test_count_all_rows();
    test_count_non_null_values();
    test_min_max_and_sum();

    if (failures != 0) {
        std::cerr << failures << " aggregate state assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
