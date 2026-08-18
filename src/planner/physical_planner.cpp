#include "planner/physical_planner.hpp"

#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>

namespace minidb::planner {
namespace {

template <typename Node>
PhysicalPlanPtr make_plan(Node node) {
    return std::make_unique<PhysicalPlan>(
        PhysicalPlan{PhysicalPlanNode{std::move(node)}});
}

template <typename... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};

template <typename... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

}  // namespace

PhysicalPlanPtr plan_physical(LogicalPlanPtr logical_plan) {
    if (!logical_plan) {
        throw std::invalid_argument{"logical plan must not be null"};
    }

    return std::visit(
        Overloaded{
            [](LogicalTableScan& scan) {
                return make_plan(PhysicalSequentialScan{scan.table_id});
            },
            [](LogicalOneRow&) {
                return make_plan(PhysicalOneRow{});
            },
            [](LogicalFilter& filter) {
                return make_plan(PhysicalFilter{
                    std::move(filter.predicate),
                    plan_physical(std::move(filter.child))});
            },
            [](LogicalSort& sort) {
                std::vector<PhysicalSortKey> keys;
                keys.reserve(sort.terms.size());
                for (const auto& term : sort.terms) {
                    const auto direction =
                        term.direction ==
                                binder::BoundOrderDirection::ascending
                            ? PhysicalSortDirection::ascending
                            : PhysicalSortDirection::descending;
                    keys.push_back(
                        PhysicalSortKey{term.column_index, direction});
                }
                return make_plan(PhysicalInMemorySort{
                    std::move(keys),
                    plan_physical(std::move(sort.child))});
            },
            [](LogicalProjection& projection) {
                return make_plan(PhysicalProjection{
                    std::move(projection.expressions),
                    plan_physical(std::move(projection.child))});
            },
            [](LogicalDistinct& distinct) {
                return make_plan(PhysicalHashDistinct{
                    plan_physical(std::move(distinct.child))});
            },
            [](LogicalLimit& limit) {
                return make_plan(PhysicalLimit{
                    limit.limit, limit.offset,
                    plan_physical(std::move(limit.child))});
            }},
        logical_plan->node);
}

}  // namespace minidb::planner
