#pragma once

#include "binder/bound_statement.hpp"
#include "execution/operator.hpp"
#include "execution/physical_operator_builder.hpp"
#include "storage/access/row.hpp"

#include <expected>
#include <vector>

namespace nessodb::execution {

[[nodiscard]] std::expected<std::vector<storage::Row>, OperatorError> execute_select(
    binder::BoundSelectStatement statement,
    std::vector<storage::Row> input_rows);

[[nodiscard]] std::expected<std::vector<storage::Row>, OperatorError>
execute_select_from_tables(binder::BoundSelectStatement statement,
                           std::vector<TableInput> table_inputs);

}  // namespace nessodb::execution
