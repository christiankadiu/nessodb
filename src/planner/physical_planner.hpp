#pragma once

#include "planner/logical_plan.hpp"
#include "planner/physical_plan.hpp"

namespace nessodb::planner {

[[nodiscard]] PhysicalPlanPtr plan_physical(LogicalPlanPtr logical_plan);

}  // namespace nessodb::planner
