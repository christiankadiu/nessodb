#include "execution/select_executor.hpp"

#include "execution/physical_operator_builder.hpp"
#include "planner/logical_planner.hpp"
#include "planner/physical_planner.hpp"

#include <utility>

namespace minidb::execution {

std::expected<std::vector<storage::Row>, OperatorError> execute_select(
    binder::BoundSelectStatement statement,
    std::vector<storage::Row> input_rows) {
    std::vector<storage::Row> output_rows;
    output_rows.reserve(input_rows.size());
    auto logical_plan = planner::plan_select(std::move(statement));
    auto physical_plan = planner::plan_physical(std::move(logical_plan));
    auto source = build_operator_tree(std::move(physical_plan),
                                      std::move(input_rows));

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

}  // namespace minidb::execution
