#include "planner/logical_planner.hpp"

#include "binder/bound_statement.hpp"
#include "common/table_id.hpp"
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

void test_table_select_plan() {
    nessodb::binder::BoundSelectStatement statement;
    statement.table_id = nessodb::common::TableId{7};
    statement.expressions.emplace_back(
        nessodb::binder::BoundColumnReferenceExpression{1, {}});
    statement.distinct = true;
    statement.where = nessodb::binder::BoundExpression{
        nessodb::binder::BoundComparisonExpression{
            nessodb::binder::BoundComparisonOperator::greater,
            std::make_unique<nessodb::binder::BoundExpression>(
                nessodb::binder::BoundColumnReferenceExpression{0, {}}),
            std::make_unique<nessodb::binder::BoundExpression>(
                nessodb::binder::BoundLiteralExpression{
                    nessodb::types::Value{std::int64_t{10}}, {}})}};
    statement.order_by.push_back(nessodb::binder::BoundOrderByTerm{
        1, nessodb::binder::BoundOrderDirection::descending});
    statement.limit = 3;
    statement.offset = 1;

    const auto plan = nessodb::planner::plan_select(std::move(statement));
    const auto* limit =
        std::get_if<nessodb::planner::LogicalLimit>(&plan->node);
    expect(limit && limit->limit == 3 && limit->offset == 1,
           "limit is the SELECT plan root");
    if (!limit) {
        return;
    }
    const auto* distinct =
        std::get_if<nessodb::planner::LogicalDistinct>(&limit->child->node);
    expect(distinct != nullptr, "distinct follows limit");
    if (!distinct) {
        return;
    }
    const auto* projection = std::get_if<nessodb::planner::LogicalProjection>(
        &distinct->child->node);
    expect(projection && projection->expressions.size() == 1,
           "projection follows distinct");
    if (!projection) {
        return;
    }
    const auto* sort =
        std::get_if<nessodb::planner::LogicalSort>(&projection->child->node);
    expect(sort && sort->terms.size() == 1,
           "sort precedes projection");
    if (!sort) {
        return;
    }
    const auto* filter =
        std::get_if<nessodb::planner::LogicalFilter>(&sort->child->node);
    expect(filter != nullptr, "filter follows the table scan");
    if (!filter) {
        return;
    }
    const auto* scan =
        std::get_if<nessodb::planner::LogicalTableScan>(&filter->child->node);
    expect(scan && scan->table_id == nessodb::common::TableId{7},
           "table scan is the SELECT plan leaf");
}

void test_literal_select_plan() {
    nessodb::binder::BoundSelectStatement statement;
    statement.expressions.emplace_back(
        nessodb::binder::BoundLiteralExpression{
            nessodb::types::Value{std::int64_t{1}}, {}});

    const auto plan = nessodb::planner::plan_select(std::move(statement));
    const auto* projection = std::get_if<nessodb::planner::LogicalProjection>(
        &plan->node);
    expect(projection != nullptr,
           "literal SELECT has a projection root");
    if (!projection) {
        return;
    }
    expect(std::holds_alternative<nessodb::planner::LogicalOneRow>(
               projection->child->node),
           "literal SELECT is evaluated over one empty row");
}

}  // namespace

int main() {
    test_table_select_plan();
    test_literal_select_plan();

    if (failures != 0) {
        std::cerr << failures << " logical planner assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
