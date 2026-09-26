#include "execution/physical_operator_builder.hpp"

#include "binder/bound_statement.hpp"
#include "common/table_id.hpp"
#include "planner/logical_planner.hpp"
#include "planner/physical_planner.hpp"
#include "storage/access/row.hpp"
#include "types/value.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
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

std::vector<nessodb::storage::Row> collect_rows(
    nessodb::execution::RowOperator& source) {
    std::vector<nessodb::storage::Row> rows;
    while (true) {
        auto batch = source.next();
        expect(batch.has_value(), "operator tree executes without error");
        if (!batch || !*batch) {
            return rows;
        }
        for (auto& row : (*batch)->rows) {
            rows.push_back(std::move(row));
        }
    }
}

void test_build_complete_operator_tree() {
    nessodb::binder::BoundSelectStatement statement;
    statement.table_id = nessodb::common::TableId{1};
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
                    nessodb::types::Value{std::int64_t{1}}, {}})}};
    statement.order_by.push_back(nessodb::binder::BoundOrderByTerm{
        1, nessodb::binder::BoundOrderDirection::descending});
    statement.limit = 2;

    auto physical = nessodb::planner::plan_physical(
        nessodb::planner::plan_select(std::move(statement)));
    std::vector<nessodb::storage::Row> input{
        {{std::int64_t{1}, std::string{"ignored"}}},
        {{std::int64_t{2}, std::string{"Alice"}}},
        {{std::int64_t{3}, std::string{"Bob"}}},
        {{std::int64_t{4}, std::string{"Bob"}}},
        {{std::int64_t{5}, std::string{"Carol"}}},
    };

    auto source = nessodb::execution::build_operator_tree(
        std::move(physical), std::move(input));
    const auto rows = collect_rows(*source);

    expect(rows.size() == 2,
           "operator tree applies filter, distinct, and limit");
    if (rows.size() == 2) {
        expect(std::get<std::string>(rows[0].values[0]) == "Carol" &&
                   std::get<std::string>(rows[1].values[0]) == "Bob",
               "operator tree applies sort before projection");
    }
}

void test_build_one_row_source() {
    nessodb::binder::BoundSelectStatement statement;
    statement.expressions.emplace_back(
        nessodb::binder::BoundLiteralExpression{
            nessodb::types::Value{std::int64_t{42}}, {}});

    auto physical = nessodb::planner::plan_physical(
        nessodb::planner::plan_select(std::move(statement)));
    auto source = nessodb::execution::build_operator_tree(
        std::move(physical), {});
    const auto rows = collect_rows(*source);

    expect(rows.size() == 1 && rows[0].values.size() == 1 &&
               std::get<std::int64_t>(rows[0].values[0]) == 42,
           "one-row source evaluates SELECT without FROM");
}

}  // namespace

int main() {
    test_build_complete_operator_tree();
    test_build_one_row_source();

    if (failures != 0) {
        std::cerr << failures << " physical operator builder assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
