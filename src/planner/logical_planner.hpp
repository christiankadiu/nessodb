#pragma once

#include "binder/bound_statement.hpp"
#include "planner/logical_plan.hpp"

namespace nessodb::planner {

[[nodiscard]] LogicalPlanPtr plan_select(
    binder::BoundSelectStatement statement);

}  // namespace nessodb::planner
