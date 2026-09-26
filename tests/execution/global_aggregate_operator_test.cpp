#include "execution/global_aggregate_operator.hpp"
#include "execution/vector_scan_operator.hpp"

#include "binder/bound_expression.hpp"
#include "storage/access/row.hpp"
#include "types/logical_type.hpp"
#include "types/value.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using nessodb::execution::AggregateComputation;
using nessodb::execution::CountAggregateState;
using nessodb::execution::CountMode;
using nessodb::execution::GlobalAggregateOperator;
using nessodb::execution::VectorScanOperator;
using nessodb::types::NullValue;

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

AggregateComputation count_all() {
    return AggregateComputation{
        std::make_unique<CountAggregateState>(CountMode::all_rows), {}, {}};
}

AggregateComputation count_column(std::size_t column_index) {
    std::vector<nessodb::binder::BoundExpression> arguments;
    arguments.emplace_back(nessodb::binder::BoundColumnReferenceExpression{
        column_index, {}, nessodb::types::LogicalType::text});
    return AggregateComputation{
        std::make_unique<CountAggregateState>(
            CountMode::non_null_values),
        std::move(arguments), {}};
}

void test_multiple_global_counts() {
    std::vector<nessodb::storage::Row> rows{
        {{{std::int64_t{1}, std::string{"Alice"}}}},
        {{{std::int64_t{2}, NullValue{}}}},
        {{{std::int64_t{3}, std::string{"Bob"}}}},
    };
    auto source = std::make_unique<VectorScanOperator>(std::move(rows), 2);
    std::vector<AggregateComputation> aggregates;
    aggregates.push_back(count_all());
    aggregates.push_back(count_column(1));
    GlobalAggregateOperator aggregate{
        std::move(source), std::move(aggregates)};

    const auto output = aggregate.next();
    expect(output && *output && (*output)->rows.size() == 1 &&
               (*output)->rows[0].values.size() == 2,
           "global aggregation emits one row for multiple states");
    if (output && *output && (*output)->rows.size() == 1) {
        expect(std::get<std::int64_t>((*output)->rows[0].values[0]) == 3 &&
                   std::get<std::int64_t>((*output)->rows[0].values[1]) == 2,
               "global counts consume every batch and preserve NULL semantics");
    }
    const auto end = aggregate.next();
    expect(end && !*end, "global aggregation emits its result only once");
}

void test_empty_input() {
    auto source = std::make_unique<VectorScanOperator>(
        std::vector<nessodb::storage::Row>{});
    std::vector<AggregateComputation> aggregates;
    aggregates.push_back(count_all());
    GlobalAggregateOperator aggregate{
        std::move(source), std::move(aggregates)};

    const auto output = aggregate.next();
    expect(output && *output && (*output)->rows.size() == 1 &&
               std::get<std::int64_t>((*output)->rows[0].values[0]) == 0,
           "global COUNT(*) over empty input returns one row containing zero");
}

void test_invalid_configuration() {
    auto source = std::make_unique<VectorScanOperator>(
        std::vector<nessodb::storage::Row>{});
    std::vector<AggregateComputation> aggregates;
    aggregates.push_back(count_column(0));
    aggregates[0].arguments.clear();

    bool rejected = false;
    try {
        GlobalAggregateOperator aggregate{
            std::move(source), std::move(aggregates)};
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    expect(rejected, "aggregate state and argument arity must agree");
}

}  // namespace

int main() {
    test_multiple_global_counts();
    test_empty_input();
    test_invalid_configuration();

    if (failures != 0) {
        std::cerr << failures
                  << " global aggregate operator assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
