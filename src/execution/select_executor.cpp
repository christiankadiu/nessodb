#include "execution/select_executor.hpp"

#include "execution/predicate_evaluator.hpp"
#include "execution/vector_scan_operator.hpp"

#include <stdexcept>
#include <utility>
#include <variant>

namespace minidb::execution {

std::vector<storage::Row> execute_select(
    const binder::BoundSelectStatement& statement,
    std::vector<storage::Row> input_rows) {
    std::vector<storage::Row> output_rows;
    output_rows.reserve(input_rows.size());
    VectorScanOperator source{std::move(input_rows)};

    while (auto batch = source.next()) {
        for (auto& input_row : batch->rows) {
            if (statement.where &&
                !matches_predicate(input_row.values, *statement.where)) {
                continue;
            }

            if (statement.expressions.empty()) {
                output_rows.push_back(std::move(input_row));
                continue;
            }

            storage::Row output_row;
            output_row.values.reserve(statement.expressions.size());
            for (const auto& expression : statement.expressions) {
                if (const auto* literal =
                        std::get_if<binder::BoundLiteralExpression>(&expression)) {
                    output_row.values.push_back(literal->value);
                    continue;
                }

                const auto column_index =
                    std::get<binder::BoundColumnReferenceExpression>(expression)
                        .column_index;
                if (column_index >= input_row.values.size()) {
                    throw std::logic_error{
                        "table schema and stored row state diverged"};
                }
                output_row.values.push_back(input_row.values[column_index]);
            }
            output_rows.push_back(std::move(output_row));
        }
    }
    return output_rows;
}

}  // namespace minidb::execution
