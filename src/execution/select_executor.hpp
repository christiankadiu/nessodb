#pragma once

#include "binder/bound_statement.hpp"
#include "storage/access/row.hpp"

#include <vector>

namespace minidb::execution {

[[nodiscard]] std::vector<storage::Row> execute_select(
    const binder::BoundSelectStatement& statement,
    std::vector<storage::Row> input_rows);

}  // namespace minidb::execution
