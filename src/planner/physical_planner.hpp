#pragma once

#include "planner/logical_plan.hpp"
#include "planner/physical_plan.hpp"

namespace minidb::planner {

[[nodiscard]] PhysicalPlanPtr plan_physical(LogicalPlanPtr logical_plan);

}  // namespace minidb::planner
