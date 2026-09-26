#include "execution/hash_aggregate_operator.hpp"
#include "execution/vector_scan_operator.hpp"

#include "binder/bound_expression.hpp"
#include "storage/access/row.hpp"
#include "types/logical_type.hpp"
#include "types/value.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using nessodb::execution::AggregateComputation;
using nessodb::execution::CountAggregateState;
using nessodb::execution::CountMode;
using nessodb::execution::HashAggregateOperator;
using nessodb::execution::OperatorErrorCode;
using nessodb::execution::SumAggregateState;
using nessodb::execution::VectorScanOperator;
using nessodb::types::LogicalType;
using nessodb::types::NullValue;

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

nessodb::binder::BoundExpression column(std::size_t index,
                                       LogicalType type) {
    return nessodb::binder::BoundExpression{
        nessodb::binder::BoundColumnReferenceExpression{index, {}, type}};
}

AggregateComputation count_all() {
    return AggregateComputation{
        std::make_unique<CountAggregateState>(CountMode::all_rows), {}, {}};
}

AggregateComputation sum_column(std::size_t index) {
    std::vector<nessodb::binder::BoundExpression> arguments;
    arguments.push_back(column(index, LogicalType::integer));
    return AggregateComputation{
        std::make_unique<SumAggregateState>(), std::move(arguments), {}};
}

void test_groups_across_batches() {
    std::vector<nessodb::storage::Row> rows{
        {{std::string{"north"}, std::int64_t{10}}},
        {{std::string{"south"}, std::int64_t{4}}},
        {{std::string{"north"}, std::int64_t{7}}},
        {{NullValue{}, std::int64_t{3}}},
        {{std::string{"south"}, NullValue{}}},
        {{NullValue{}, std::int64_t{2}}},
    };
    auto source = std::make_unique<VectorScanOperator>(std::move(rows), 2);
    std::vector<nessodb::binder::BoundExpression> keys;
    keys.push_back(column(0, LogicalType::text));
    std::vector<AggregateComputation> aggregates;
    aggregates.push_back(count_all());
    aggregates.push_back(sum_column(1));
    HashAggregateOperator aggregate{
        std::move(source), std::move(keys), std::move(aggregates), 2};

    const auto first = aggregate.next();
    const auto second = aggregate.next();
    const auto end = aggregate.next();
    expect(first && *first && (*first)->rows.size() == 2 &&
               second && *second && (*second)->rows.size() == 1,
           "hash aggregation emits groups in bounded batches");
    if (first && *first && (*first)->rows.size() == 2 &&
        second && *second && (*second)->rows.size() == 1) {
        const auto& north = (*first)->rows[0].values;
        const auto& south = (*first)->rows[1].values;
        const auto& null_group = (*second)->rows[0].values;
        expect(std::get<std::string>(north[0]) == "north" &&
                   std::get<std::int64_t>(north[1]) == 2 &&
                   std::get<std::int64_t>(north[2]) == 17 &&
                   std::get<std::string>(south[0]) == "south" &&
                   std::get<std::int64_t>(south[1]) == 2 &&
                   std::get<std::int64_t>(south[2]) == 4,
               "each group maintains independent aggregate states");
        expect(std::holds_alternative<NullValue>(null_group[0]) &&
                   std::get<std::int64_t>(null_group[1]) == 2 &&
                   std::get<std::int64_t>(null_group[2]) == 5,
               "NULL keys form one group");
    }
    expect(end && !*end,
           "hash aggregation signals exhaustion after all groups");
}

void test_grouping_without_aggregates_and_empty_input() {
    std::vector<nessodb::storage::Row> rows{
        {{std::int64_t{2}}},
        {{std::int64_t{1}}},
        {{std::int64_t{2}}},
    };
    auto source = std::make_unique<VectorScanOperator>(std::move(rows));
    std::vector<nessodb::binder::BoundExpression> keys;
    keys.push_back(column(0, LogicalType::integer));
    HashAggregateOperator grouping{
        std::move(source), std::move(keys), {}};
    const auto output = grouping.next();
    expect(output && *output && (*output)->rows.size() == 2 &&
               std::get<std::int64_t>((*output)->rows[0].values[0]) == 2 &&
               std::get<std::int64_t>((*output)->rows[1].values[0]) == 1,
           "group-only aggregation preserves first-seen key order");

    auto empty_source = std::make_unique<VectorScanOperator>(
        std::vector<nessodb::storage::Row>{});
    std::vector<nessodb::binder::BoundExpression> empty_keys;
    empty_keys.push_back(column(0, LogicalType::integer));
    HashAggregateOperator empty{
        std::move(empty_source), std::move(empty_keys), {}};
    const auto empty_output = empty.next();
    expect(empty_output && !*empty_output,
           "grouped aggregation over empty input emits no rows");
}

void test_memory_limit() {
    std::vector<nessodb::storage::Row> rows{
        {{std::string{"group"}}},
    };
    auto source = std::make_unique<VectorScanOperator>(std::move(rows));
    std::vector<nessodb::binder::BoundExpression> keys;
    keys.push_back(column(0, LogicalType::text));
    HashAggregateOperator limited{
        std::move(source), std::move(keys), {},
        nessodb::execution::default_batch_size, 1};
    const auto result = limited.next();
    expect(!result && result.error().code ==
                          OperatorErrorCode::memory_limit_exceeded,
           "hash aggregation reports an exhausted memory budget");
}

}  // namespace

int main() {
    test_groups_across_batches();
    test_grouping_without_aggregates_and_empty_input();
    test_memory_limit();

    if (failures != 0) {
        std::cerr << failures
                  << " hash aggregate operator assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
