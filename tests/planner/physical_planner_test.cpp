#include "planner/physical_planner.hpp"

#include "binder/bound_statement.hpp"
#include "common/table_id.hpp"
#include "planner/logical_planner.hpp"
#include "types/value.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <utility>

namespace {

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

void test_physical_algorithm_selection() {
    nessodb::binder::BoundSelectStatement statement;
    statement.table_id = nessodb::common::TableId{11};
    statement.expressions.emplace_back(
        nessodb::binder::BoundColumnReferenceExpression{1, {}});
    statement.distinct = true;
    statement.where = nessodb::binder::BoundExpression{
        nessodb::binder::BoundComparisonExpression{
            nessodb::binder::BoundComparisonOperator::greater_equal,
            std::make_unique<nessodb::binder::BoundExpression>(
                nessodb::binder::BoundColumnReferenceExpression{0, {}}),
            std::make_unique<nessodb::binder::BoundExpression>(
                nessodb::binder::BoundLiteralExpression{
                    nessodb::types::Value{std::int64_t{18}}, {}})}};
    statement.order_by.push_back(nessodb::binder::BoundOrderByTerm{
        1, nessodb::binder::BoundOrderDirection::descending});
    statement.limit = 5;
    statement.offset = 2;

    auto logical = nessodb::planner::plan_select(std::move(statement));
    const auto physical =
        nessodb::planner::plan_physical(std::move(logical));

    const auto* limit =
        std::get_if<nessodb::planner::PhysicalLimit>(&physical->node);
    expect(limit && limit->limit == 5 && limit->offset == 2,
           "physical limit preserves its bounds");
    if (!limit) {
        return;
    }
    const auto* distinct =
        std::get_if<nessodb::planner::PhysicalHashDistinct>(
            &limit->child->node);
    expect(distinct != nullptr, "logical distinct selects hash distinct");
    if (!distinct) {
        return;
    }
    const auto* projection =
        std::get_if<nessodb::planner::PhysicalProjection>(
            &distinct->child->node);
    expect(projection && projection->expressions.size() == 1,
           "physical projection owns its expressions");
    if (!projection) {
        return;
    }
    const auto* sort =
        std::get_if<nessodb::planner::PhysicalInMemorySort>(
            &projection->child->node);
    expect(sort && sort->keys.size() == 1 &&
               sort->keys.front().direction ==
                   nessodb::planner::PhysicalSortDirection::descending,
           "logical sort selects descending in-memory sort");
    if (!sort) {
        return;
    }
    const auto* filter =
        std::get_if<nessodb::planner::PhysicalFilter>(&sort->child->node);
    expect(filter != nullptr, "physical filter is retained");
    if (!filter) {
        return;
    }
    const auto* scan =
        std::get_if<nessodb::planner::PhysicalSequentialScan>(
            &filter->child->node);
    expect(scan && scan->table_id == nessodb::common::TableId{11},
           "table scan selects sequential access");
}

void test_one_row_selection() {
    nessodb::binder::BoundSelectStatement statement;
    statement.expressions.emplace_back(
        nessodb::binder::BoundLiteralExpression{
            nessodb::types::Value{std::int64_t{1}}, {}});

    auto logical = nessodb::planner::plan_select(std::move(statement));
    const auto physical =
        nessodb::planner::plan_physical(std::move(logical));
    const auto* projection =
        std::get_if<nessodb::planner::PhysicalProjection>(&physical->node);
    expect(projection != nullptr, "literal SELECT retains projection");
    if (!projection) {
        return;
    }
    expect(std::holds_alternative<nessodb::planner::PhysicalOneRow>(
               projection->child->node),
           "one-row input remains a dedicated physical source");
}

}  // namespace

int main() {
    test_physical_algorithm_selection();
    test_one_row_selection();

    if (failures != 0) {
        std::cerr << failures << " physical planner assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
