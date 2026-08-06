#pragma once

#include "binder/bound_statement.hpp"
#include "planner/logical_plan.hpp"

namespace minidb::planner {

[[nodiscard]] LogicalPlanPtr plan_select(
    binder::BoundSelectStatement statement);

}  // namespace minidb::planner
