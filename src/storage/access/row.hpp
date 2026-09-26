#pragma once

#include "types/value.hpp"

#include <vector>

namespace nessodb::storage {

struct Row {
    std::vector<types::Value> values;
};

}  // namespace nessodb::storage
