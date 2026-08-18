#pragma once

#include "planner/physical_plan.hpp"

#include <string>
#include <vector>

namespace minidb::planner {

[[nodiscard]] std::vector<std::string> format_physical_plan(
    const PhysicalPlan& plan);

}  // namespace minidb::planner
