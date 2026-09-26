#pragma once

#include "common/table_id.hpp"
#include "execution/operator.hpp"
#include "planner/physical_plan.hpp"
#include "storage/access/row.hpp"

#include <memory>
#include <vector>

namespace nessodb::execution {

struct TableInput {
    common::TableId table_id;
    std::vector<storage::Row> rows;
};

[[nodiscard]] std::unique_ptr<RowOperator> build_operator_tree(
    planner::PhysicalPlanPtr plan,
    std::vector<storage::Row> input_rows);

[[nodiscard]] std::unique_ptr<RowOperator> build_operator_tree_for_tables(
    planner::PhysicalPlanPtr plan,
    std::vector<TableInput> table_inputs);

}  // namespace nessodb::execution
