#include "execution/physical_operator_builder.hpp"

#include "execution/distinct_operator.hpp"
#include "execution/filter_operator.hpp"
#include "execution/global_aggregate_operator.hpp"
#include "execution/hash_aggregate_operator.hpp"
#include "execution/limit_operator.hpp"
#include "execution/nested_loop_join_operator.hpp"
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

std::unique_ptr<RowOperator> build_operator_tree_impl(
    planner::PhysicalPlanPtr plan,
    std::vector<TableInput>& table_inputs) {
    if (!plan) {
        throw std::invalid_argument{"physical plan must not be null"};
    }

    return std::visit(
        Overloaded{
            [&table_inputs](planner::PhysicalSequentialScan& scan)
                -> std::unique_ptr<RowOperator> {
                auto input = table_inputs.begin();
                while (input != table_inputs.end() &&
                       input->table_id != scan.table_id) {
                    ++input;
                }
                if (input == table_inputs.end() &&
                    table_inputs.size() == 1 &&
                    !table_inputs.front().table_id.is_valid()) {
                    input = table_inputs.begin();
                }
                if (input == table_inputs.end()) {
                    throw std::logic_error{
                        "physical scan has no matching table input"};
                }
                auto rows = std::move(input->rows);
                table_inputs.erase(input);
                return std::make_unique<VectorScanOperator>(
                    std::move(rows));
            },
            [](planner::PhysicalOneRow&)
                -> std::unique_ptr<RowOperator> {
                return std::make_unique<VectorScanOperator>(
                    std::vector<storage::Row>(1));
            },
            [&table_inputs](planner::PhysicalFilter& filter)
                -> std::unique_ptr<RowOperator> {
                auto child = build_operator_tree_impl(
                    std::move(filter.child), table_inputs);
                return std::make_unique<FilterOperator>(
                    std::move(child), std::move(filter.predicate));
            },
            [&table_inputs](planner::PhysicalNestedLoopJoin& join)
                -> std::unique_ptr<RowOperator> {
                auto left = build_operator_tree_impl(
                    std::move(join.left), table_inputs);
                auto right = build_operator_tree_impl(
                    std::move(join.right), table_inputs);
                return std::make_unique<NestedLoopJoinOperator>(
                    std::move(left), std::move(right),
                    std::move(join.predicate));
            },
            [&table_inputs](planner::PhysicalInMemorySort& sort)
                -> std::unique_ptr<RowOperator> {
                auto child = build_operator_tree_impl(
                    std::move(sort.child), table_inputs);
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
            [&table_inputs](planner::PhysicalProjection& projection)
                -> std::unique_ptr<RowOperator> {
                auto child = build_operator_tree_impl(
                    std::move(projection.child), table_inputs);
                return std::make_unique<ProjectionOperator>(
                    std::move(child), std::move(projection.expressions));
            },
            [&table_inputs](planner::PhysicalGlobalAggregate& aggregate)
                -> std::unique_ptr<RowOperator> {
                auto child = build_operator_tree_impl(
                    std::move(aggregate.child), table_inputs);
                return std::make_unique<GlobalAggregateOperator>(
                    std::move(child), build_aggregate_computations(
                                          std::move(aggregate.aggregates)));
            },
            [&table_inputs](planner::PhysicalHashAggregate& aggregate)
                -> std::unique_ptr<RowOperator> {
                auto child = build_operator_tree_impl(
                    std::move(aggregate.child), table_inputs);
                return std::make_unique<HashAggregateOperator>(
                    std::move(child), std::move(aggregate.group_keys),
                    build_aggregate_computations(
                        std::move(aggregate.aggregates)));
            },
            [&table_inputs](planner::PhysicalHashDistinct& distinct)
                -> std::unique_ptr<RowOperator> {
                auto child = build_operator_tree_impl(
                    std::move(distinct.child), table_inputs);
                return std::make_unique<DistinctOperator>(std::move(child));
            },
            [&table_inputs](planner::PhysicalLimit& limit)
                -> std::unique_ptr<RowOperator> {
                auto child = build_operator_tree_impl(
                    std::move(limit.child), table_inputs);
                return std::make_unique<LimitOperator>(
                    std::move(child), limit.limit, limit.offset);
            }},
        plan->node);
}

}  // namespace

std::unique_ptr<RowOperator> build_operator_tree(
    planner::PhysicalPlanPtr plan,
    std::vector<storage::Row> input_rows) {
    std::vector<TableInput> inputs;
    inputs.push_back(TableInput{{}, std::move(input_rows)});
    return build_operator_tree_impl(std::move(plan), inputs);
}

std::unique_ptr<RowOperator> build_operator_tree_for_tables(
    planner::PhysicalPlanPtr plan,
    std::vector<TableInput> table_inputs) {
    return build_operator_tree_impl(std::move(plan), table_inputs);
}

}  // namespace minidb::execution
