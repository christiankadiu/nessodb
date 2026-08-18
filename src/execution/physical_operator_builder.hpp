#pragma once

#include "execution/operator.hpp"
#include "planner/physical_plan.hpp"
#include "storage/access/row.hpp"

#include <memory>
#include <vector>

namespace minidb::execution {

[[nodiscard]] std::unique_ptr<RowOperator> build_operator_tree(
    planner::PhysicalPlanPtr plan,
    std::vector<storage::Row> input_rows);

}  // namespace minidb::execution
