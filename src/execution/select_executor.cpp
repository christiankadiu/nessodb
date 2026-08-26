#include "execution/select_executor.hpp"

#include "execution/physical_operator_builder.hpp"
#include "planner/logical_planner.hpp"
#include "planner/physical_planner.hpp"

#include <utility>

namespace minidb::execution {
namespace {

std::expected<std::vector<storage::Row>, OperatorError> collect_rows(
    std::unique_ptr<RowOperator> source, std::size_t reserve) {
    std::vector<storage::Row> output_rows;
    output_rows.reserve(reserve);
    while (true) {
        auto batch = source->next();
        if (!batch) {
            return std::unexpected(batch.error());
        }
        if (!*batch) {
            break;
        }
        for (auto& row : (*batch)->rows) {
            output_rows.push_back(std::move(row));
        }
    }
    return output_rows;
}

}  // namespace

std::expected<std::vector<storage::Row>, OperatorError>
execute_select(
    binder::BoundSelectStatement statement,
    std::vector<storage::Row> input_rows) {
    const auto reserve = input_rows.size();
    auto logical_plan = planner::plan_select(std::move(statement));
    auto physical_plan = planner::plan_physical(std::move(logical_plan));
    auto source = build_operator_tree(std::move(physical_plan),
                                      std::move(input_rows));

    return collect_rows(std::move(source), reserve);
}

std::expected<std::vector<storage::Row>, OperatorError>
execute_select_from_tables(
    binder::BoundSelectStatement statement,
    std::vector<TableInput> table_inputs) {
    std::size_t reserve = 0;
    for (const auto& input : table_inputs) {
        reserve += input.rows.size();
    }
    auto logical_plan = planner::plan_select(std::move(statement));
    auto physical_plan = planner::plan_physical(std::move(logical_plan));
    auto source = build_operator_tree_for_tables(
        std::move(physical_plan), std::move(table_inputs));
    return collect_rows(std::move(source), reserve);
}

}  // namespace minidb::execution
