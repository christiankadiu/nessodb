#include "execution/select_executor.hpp"

#include "execution/distinct_operator.hpp"
#include "execution/filter_operator.hpp"
#include "execution/limit_operator.hpp"
#include "execution/projection_operator.hpp"
#include "execution/sort_operator.hpp"
#include "execution/vector_scan_operator.hpp"

#include <limits>
#include <memory>
#include <utility>

namespace minidb::execution {

std::expected<std::vector<storage::Row>, OperatorError> execute_select(
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
    if (!statement.order_by.empty()) {
        std::vector<SortKey> keys;
        keys.reserve(statement.order_by.size());
        for (const auto& term : statement.order_by) {
            const auto direction =
                term.direction == binder::BoundOrderDirection::ascending
                    ? SortDirection::ascending
                    : SortDirection::descending;
            keys.push_back(SortKey{term.column_index, direction});
        }
        source = std::make_unique<SortOperator>(std::move(source),
                                                std::move(keys));
    }
    if (!statement.expressions.empty()) {
        source = std::make_unique<ProjectionOperator>(
            std::move(source), statement.expressions);
    }
    if (statement.distinct) {
        source = std::make_unique<DistinctOperator>(std::move(source));
    }
    if (statement.limit || statement.offset) {
        source = std::make_unique<LimitOperator>(
            std::move(source),
            statement.limit.value_or(
                std::numeric_limits<std::size_t>::max()),
            statement.offset.value_or(0));
    }

    while (true) {
        auto batch = source->next();
        if (!batch) {
            return std::unexpected(batch.error());
        }
        if (!*batch) {
            break;
        }
        for (auto& row : (*batch)->rows) {
            output_rows.push_back(std::move(row));
        }
    }
    return output_rows;
}

}  // namespace minidb::execution
