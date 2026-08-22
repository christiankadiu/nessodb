#include "execution/physical_operator_builder.hpp"

#include "execution/distinct_operator.hpp"
#include "execution/filter_operator.hpp"
#include "execution/global_aggregate_operator.hpp"
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
                std::vector<AggregateComputation> computations;
                computations.reserve(aggregate.aggregates.size());
                for (auto& expression : aggregate.aggregates) {
                    std::unique_ptr<AggregateState> state;
                    switch (expression.function) {
                        case binder::BoundAggregateFunction::count: {
                            const auto mode = expression.arguments.empty()
                                                  ? CountMode::all_rows
                                                  : CountMode::non_null_values;
                            state = std::make_unique<CountAggregateState>(mode);
                            break;
                        }
                        case binder::BoundAggregateFunction::minimum:
                            state = std::make_unique<MinMaxAggregateState>(
                                MinMaxMode::minimum);
                            break;
                        case binder::BoundAggregateFunction::maximum:
                            state = std::make_unique<MinMaxAggregateState>(
                                MinMaxMode::maximum);
                            break;
                        case binder::BoundAggregateFunction::sum:
                            state = std::make_unique<SumAggregateState>();
                            break;
                    }
                    computations.push_back(AggregateComputation{
                        std::move(state),
                        std::move(expression.arguments),
                        expression.location});
                }
                return std::make_unique<GlobalAggregateOperator>(
                    std::move(child), std::move(computations));
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
