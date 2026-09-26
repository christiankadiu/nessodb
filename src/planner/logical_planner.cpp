#include "planner/logical_planner.hpp"

#include <limits>
#include <memory>
#include <utility>

namespace nessodb::planner {
namespace {

template <typename Node>
LogicalPlanPtr make_plan(Node node) {
    return std::make_unique<LogicalPlan>(
        LogicalPlan{LogicalPlanNode{std::move(node)}});
}

}  // namespace

LogicalPlanPtr plan_select(binder::BoundSelectStatement statement) {
    LogicalPlanPtr plan = statement.table_id.is_valid()
                              ? make_plan(LogicalTableScan{
                                    statement.table_id,
                                    std::move(statement.primary_key_lookup)})
                              : make_plan(LogicalOneRow{});

    for (auto& join : statement.joins) {
        auto right = make_plan(LogicalTableScan{join.table_id, std::nullopt});
        plan = make_plan(LogicalNestedLoopJoin{
            std::move(join.condition), std::move(plan),
            std::move(right)});
    }

    if (statement.where) {
        plan = make_plan(LogicalFilter{
            std::move(*statement.where), std::move(plan)});
    }
    if (!statement.group_by.empty()) {
        plan = make_plan(LogicalAggregate{
            std::move(statement.group_by),
            std::move(statement.aggregates), std::move(plan)});
        if (!statement.order_by.empty()) {
            plan = make_plan(LogicalSort{
                std::move(statement.order_by), std::move(plan)});
        }
        if (!statement.expressions.empty()) {
            plan = make_plan(LogicalProjection{
                std::move(statement.expressions), std::move(plan)});
        }
    } else {
        if (!statement.order_by.empty()) {
            plan = make_plan(LogicalSort{
                std::move(statement.order_by), std::move(plan)});
        }
        if (!statement.expressions.empty()) {
            plan = make_plan(LogicalProjection{
                std::move(statement.expressions), std::move(plan)});
        }
        if (!statement.aggregates.empty()) {
            plan = make_plan(LogicalAggregate{
                {}, std::move(statement.aggregates), std::move(plan)});
        }
    }
    if (statement.distinct) {
        plan = make_plan(LogicalDistinct{std::move(plan)});
    }
    if (statement.limit || statement.offset) {
        plan = make_plan(LogicalLimit{
            statement.limit.value_or(
                std::numeric_limits<std::size_t>::max()),
            statement.offset.value_or(0), std::move(plan)});
    }
    return plan;
}

}  // namespace nessodb::planner
