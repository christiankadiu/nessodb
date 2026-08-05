#include "execution/select_executor.hpp"

#include "execution/filter_operator.hpp"
#include "execution/projection_operator.hpp"
#include "execution/vector_scan_operator.hpp"

#include <memory>
#include <utility>

namespace minidb::execution {

std::vector<storage::Row> execute_select(
    const binder::BoundSelectStatement& statement,
    std::vector<storage::Row> input_rows) {
    std::vector<storage::Row> output_rows;
    output_rows.reserve(input_rows.size());
    std::unique_ptr<RowOperator> source =
        std::make_unique<VectorScanOperator>(std::move(input_rows));
    if (statement.where) {
        source = std::make_unique<FilterOperator>(std::move(source),
                                                  *statement.where);
    }
    if (!statement.expressions.empty()) {
        source = std::make_unique<ProjectionOperator>(
            std::move(source), statement.expressions);
    }

    while (auto batch = source->next()) {
        for (auto& row : batch->rows) {
            output_rows.push_back(std::move(row));
        }
    }
    return output_rows;
}

}  // namespace minidb::execution
