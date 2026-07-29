#pragma once

#include "common/table_id.hpp"
#include "types/logical_type.hpp"

#include <string>
#include <vector>

namespace minidb::catalog {

struct ColumnSchema {
    std::string name;
    types::LogicalType type;
};

struct TableSchema {
    std::string name;
    std::vector<ColumnSchema> columns;
    common::TableId id{};
};

}  // namespace minidb::catalog
