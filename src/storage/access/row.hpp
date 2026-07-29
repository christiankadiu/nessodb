#pragma once

#include "types/value.hpp"

#include <vector>

namespace minidb::storage {

struct Row {
    std::vector<types::Value> values;
};

}  // namespace minidb::storage
