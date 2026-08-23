#include "execution/physical_operator_builder.hpp"

#include "execution/distinct_operator.hpp"
#include "execution/filter_operator.hpp"
#include "execution/global_aggregate_operator.hpp"
#include "execution/hash_aggregate_operator.hpp"
#include "execution/limit_operator.hpp"
#include "execution/projection_operator.hpp"
#include "execution/sort_operator.hpp"
#include "execution/vector_scan_operator.hpp"

#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace minidb::execution {
namespace {

template <typename... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};

template <typename... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

std::unique_ptr<AggregateState> build_aggregate_state(
    const binder::BoundAggregateExpression& expression) {
    switch (expression.function) {
        case binder::BoundAggregateFunction::count: {
            const auto mode = expression.arguments.empty()
                                  ? CountMode::all_rows
                                  : CountMode::non_null_values;
            return std::make_unique<CountAggregateState>(mode);
        }
        case binder::BoundAggregateFunction::minimum:
            return std::make_unique<MinMaxAggregateState>(
                MinMaxMode::minimum);
        case binder::BoundAggregateFunction::maximum:
            return std::make_unique<MinMaxAggregateState>(
                MinMaxMode::maximum);
        case binder::BoundAggregateFunction::sum:
            return std::make_unique<SumAggregateState>();
    }
    throw std::logic_error{"unknown aggregate function"};
}

std::vector<AggregateComputation> build_aggregate_computations(
    std::vector<binder::BoundAggregateExpression> expressions) {
    std::vector<AggregateComputation> computations;
    computations.reserve(expressions.size());
    for (auto& expression : expressions) {
        computations.push_back(AggregateComputation{
            build_aggregate_state(expression),
            std::move(expression.arguments), expression.location});
    }
    return computations;
}

}  // namespace

std::unique_ptr<RowOperator> build_operator_tree(
    planner::PhysicalPlanPtr plan,
    std::vector<storage::Row> input_rows) {
    if (!plan) {
        throw std::invalid_argument{"physical plan must not be null"};
    }

    return std::visit(
        Overloaded{
            [&input_rows](planner::PhysicalSequentialScan&)
                -> std::unique_ptr<RowOperator> {
                return std::make_unique<VectorScanOperator>(
                    std::move(input_rows));
            },
            [](planner::PhysicalOneRow&)
                -> std::unique_ptr<RowOperator> {
                return std::make_unique<VectorScanOperator>(
                    std::vector<storage::Row>(1));
            },
            [&input_rows](planner::PhysicalFilter& filter)
                -> std::unique_ptr<RowOperator> {
                auto child = build_operator_tree(
                    std::move(filter.child), std::move(input_rows));
                return std::make_unique<FilterOperator>(
                    std::move(child), std::move(filter.predicate));
            },
            [&input_rows](planner::PhysicalInMemorySort& sort)
                -> std::unique_ptr<RowOperator> {
                auto child = build_operator_tree(
                    std::move(sort.child), std::move(input_rows));
                std::vector<SortKey> keys;
                keys.reserve(sort.keys.size());
                for (const auto& key : sort.keys) {
                    const auto direction =
                        key.direction ==
                                planner::PhysicalSortDirection::ascending
                            ? SortDirection::ascending
                            : SortDirection::descending;
                    keys.push_back(SortKey{key.column_index, direction});
                }
                return std::make_unique<SortOperator>(
                    std::move(child), std::move(keys));
            },
            [&input_rows](planner::PhysicalProjection& projection)
                -> std::unique_ptr<RowOperator> {
                auto child = build_operator_tree(
                    std::move(projection.child), std::move(input_rows));
                return std::make_unique<ProjectionOperator>(
                    std::move(child), std::move(projection.expressions));
            },
            [&input_rows](planner::PhysicalGlobalAggregate& aggregate)
                -> std::unique_ptr<RowOperator> {
                auto child = build_operator_tree(
                    std::move(aggregate.child), std::move(input_rows));
                return std::make_unique<GlobalAggregateOperator>(
                    std::move(child), build_aggregate_computations(
                                          std::move(aggregate.aggregates)));
            },
            [&input_rows](planner::PhysicalHashAggregate& aggregate)
                -> std::unique_ptr<RowOperator> {
                auto child = build_operator_tree(
                    std::move(aggregate.child), std::move(input_rows));
                return std::make_unique<HashAggregateOperator>(
                    std::move(child), std::move(aggregate.group_keys),
                    build_aggregate_computations(
                        std::move(aggregate.aggregates)));
            },
            [&input_rows](planner::PhysicalHashDistinct& distinct)
                -> std::unique_ptr<RowOperator> {
                auto child = build_operator_tree(
                    std::move(distinct.child), std::move(input_rows));
                return std::make_unique<DistinctOperator>(std::move(child));
            },
            [&input_rows](planner::PhysicalLimit& limit)
                -> std::unique_ptr<RowOperator> {
                auto child = build_operator_tree(
                    std::move(limit.child), std::move(input_rows));
                return std::make_unique<LimitOperator>(
                    std::move(child), limit.limit, limit.offset);
            }},
        plan->node);
}

}  // namespace minidb::execution
