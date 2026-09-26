#pragma once

#include "planner/physical_plan.hpp"

#include <string>
#include <vector>

namespace nessodb::planner {

[[nodiscard]] std::vector<std::string> format_physical_plan(
    const PhysicalPlan& plan);

}  // namespace nessodb::planner
