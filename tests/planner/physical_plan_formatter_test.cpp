#include "planner/physical_plan_formatter.hpp"

#include "binder/bound_statement.hpp"
#include "common/table_id.hpp"
#include "planner/logical_planner.hpp"
#include "planner/physical_planner.hpp"
#include "types/value.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

void test_format_operator_tree() {
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

    const auto plan = nessodb::planner::plan_physical(
        nessodb::planner::plan_select(std::move(statement)));
    const auto lines = nessodb::planner::format_physical_plan(*plan);
    const std::vector<std::string> expected{
        "Limit (limit=5, offset=2)",
        "  Hash Distinct",
        "    Projection (expressions=1)",
        "      In-Memory Sort (keys=column[1] DESC)",
        "        Filter",
        "          Sequential Scan (table_id=11)",
    };

    expect(lines == expected,
           "physical plan format preserves hierarchy and parameters");
}

void test_format_one_row_plan() {
    nessodb::binder::BoundSelectStatement statement;
    statement.expressions.emplace_back(
        nessodb::binder::BoundLiteralExpression{
            nessodb::types::Value{std::int64_t{1}}, {}});

    const auto plan = nessodb::planner::plan_physical(
        nessodb::planner::plan_select(std::move(statement)));
    const auto lines = nessodb::planner::format_physical_plan(*plan);
    const std::vector<std::string> expected{
        "Projection (expressions=1)",
        "  One Row",
    };

    expect(lines == expected,
           "physical plan format represents SELECT without FROM");
}

}  // namespace

int main() {
    test_format_operator_tree();
    test_format_one_row_plan();

    if (failures != 0) {
        std::cerr << failures << " physical plan formatter assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
