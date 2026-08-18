#pragma once

#include "binder/bound_statement.hpp"
#include "execution/operator.hpp"
#include "storage/access/row.hpp"

#include <expected>
#include <vector>

namespace minidb::execution {

[[nodiscard]] std::expected<std::vector<storage::Row>, OperatorError> execute_select(
    binder::BoundSelectStatement statement,
    std::vector<storage::Row> input_rows);

}  // namespace minidb::execution
