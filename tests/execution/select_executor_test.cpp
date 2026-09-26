#include "execution/select_executor.hpp"
#include "execution/distinct_operator.hpp"
#include "execution/filter_operator.hpp"
#include "execution/limit_operator.hpp"
#include "execution/projection_operator.hpp"
#include "execution/sort_operator.hpp"
#include "execution/vector_scan_operator.hpp"

#include "binder/bound_statement.hpp"
#include "common/table_id.hpp"
#include "storage/access/row.hpp"
#include "types/value.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

nessodb::binder::BoundExpression comparison_expression(
    std::size_t column_index,
    nessodb::binder::BoundComparisonOperator comparison,
    nessodb::types::Value value) {
    return nessodb::binder::BoundExpression{
        nessodb::binder::BoundComparisonExpression{
            comparison,
            std::make_unique<nessodb::binder::BoundExpression>(
                nessodb::binder::BoundColumnReferenceExpression{
                    column_index, {}}),
            std::make_unique<nessodb::binder::BoundExpression>(
                nessodb::binder::BoundLiteralExpression{
                    std::move(value), {}})}};
}

std::optional<nessodb::execution::RowBatch> next_batch(
    nessodb::execution::RowOperator& row_operator) {
    auto result = row_operator.next();
    expect(result.has_value(), "row operator completes without runtime error");
    if (!result) {
        return std::nullopt;
    }
    return std::move(*result);
}

void test_filter_and_projection() {
    nessodb::binder::BoundSelectStatement statement;
    statement.table_id = nessodb::common::TableId{1};
    statement.expressions.emplace_back(
        nessodb::binder::BoundColumnReferenceExpression{1, {}});
    statement.expressions.emplace_back(
        nessodb::binder::BoundLiteralExpression{
            nessodb::types::Value{std::string{"active"}}, {}});
    statement.where = comparison_expression(
        0, nessodb::binder::BoundComparisonOperator::greater,
        nessodb::types::Value{std::int64_t{1}});

    std::vector<nessodb::storage::Row> rows{
        {{{std::int64_t{1}, std::string{"Alice"}}}},
        {{{std::int64_t{2}, std::string{"Bob"}}}},
    };
    const auto output =
        nessodb::execution::execute_select(std::move(statement),
                                          std::move(rows));

    expect(output && output->size() == 1 &&
               output->front().values.size() == 2,
           "SELECT executor filters and projects rows");
    if (output && output->size() == 1 &&
        output->front().values.size() == 2) {
        expect(std::get<std::string>(output->front().values[0]) == "Bob" &&
                   std::get<std::string>(output->front().values[1]) == "active",
               "SELECT executor emits bound columns and literals");
    }
}

void test_wildcard_and_literal_selects() {
    nessodb::binder::BoundSelectStatement wildcard;
    wildcard.table_id = nessodb::common::TableId{1};
    std::vector<nessodb::storage::Row> rows{
        {{{std::int64_t{1}, std::string{"Alice"}}}},
    };
    const auto scanned =
        nessodb::execution::execute_select(std::move(wildcard),
                                          std::move(rows));
    expect(scanned && scanned->size() == 1 &&
               scanned->front().values.size() == 2,
           "empty projection preserves wildcard rows");

    nessodb::binder::BoundSelectStatement literals;
    literals.expressions.emplace_back(
        nessodb::binder::BoundLiteralExpression{
            nessodb::types::Value{std::int64_t{42}}, {}});
    std::vector<nessodb::storage::Row> single_input(1);
    const auto selected = nessodb::execution::execute_select(
        std::move(literals), std::move(single_input));
    expect(selected && selected->size() == 1 &&
               std::get<std::int64_t>(
                   selected->front().values.front()) == 42,
           "literal SELECT executes from one empty input row");
}

void test_vector_scan_batches() {
    std::vector<nessodb::storage::Row> rows;
    for (std::int64_t value = 0; value < 5; ++value) {
        rows.push_back(nessodb::storage::Row{{value}});
    }
    nessodb::execution::VectorScanOperator source{std::move(rows), 2};

    const auto first = next_batch(source);
    const auto second = next_batch(source);
    const auto third = next_batch(source);
    const auto end = next_batch(source);
    expect(first && first->rows.size() == 2 &&
               second && second->rows.size() == 2 &&
               third && third->rows.size() == 1,
           "vector scan emits bounded row batches");
    expect(!end, "vector scan signals exhaustion");
}

void test_filter_operator() {
    std::vector<nessodb::storage::Row> rows{
        {{std::int64_t{0}}},
        {{std::int64_t{1}}},
        {{std::int64_t{2}}},
    };
    auto source = std::make_unique<nessodb::execution::VectorScanOperator>(
        std::move(rows), 1);
    auto predicate = comparison_expression(
        0, nessodb::binder::BoundComparisonOperator::greater,
        nessodb::types::Value{std::int64_t{1}});
    nessodb::execution::FilterOperator filter{
        std::move(source), std::move(predicate)};

    const auto output = next_batch(filter);
    expect(output && output->rows.size() == 1 &&
               std::get<std::int64_t>(output->rows.front().values.front()) == 2,
           "filter skips empty child batches and preserves matching rows");
    expect(!next_batch(filter),
           "filter signals exhaustion after matching rows");
}

void test_projection_operator() {
    std::vector<nessodb::storage::Row> rows{
        {{std::int64_t{7}, std::string{"Alice"}}},
    };
    auto source = std::make_unique<nessodb::execution::VectorScanOperator>(
        std::move(rows));
    std::vector<nessodb::binder::BoundExpression> expressions;
    expressions.emplace_back(
        nessodb::binder::BoundColumnReferenceExpression{1, {}});
    expressions.emplace_back(nessodb::binder::BoundLiteralExpression{
        nessodb::types::Value{std::string{"active"}}, {}});
    expressions.emplace_back(
        nessodb::binder::BoundColumnReferenceExpression{0, {}});
    nessodb::execution::ProjectionOperator projection{
        std::move(source), std::move(expressions)};

    const auto output = next_batch(projection);
    expect(output && output->rows.size() == 1 &&
               output->rows.front().values.size() == 3 &&
               std::get<std::string>(output->rows.front().values[0]) ==
                   "Alice" &&
               std::get<std::string>(output->rows.front().values[1]) ==
                   "active" &&
               std::get<std::int64_t>(output->rows.front().values[2]) == 7,
           "projection emits bound expressions in their requested order");
    expect(!next_batch(projection),
           "projection signals exhaustion with its child");
}

void test_limit_operator() {
    std::vector<nessodb::storage::Row> rows;
    for (std::int64_t value = 0; value < 6; ++value) {
        rows.push_back(nessodb::storage::Row{{value}});
    }
    auto source = std::make_unique<nessodb::execution::VectorScanOperator>(
        std::move(rows), 2);
    nessodb::execution::LimitOperator limit{std::move(source), 3, 2};

    const auto first = next_batch(limit);
    const auto second = next_batch(limit);
    expect(first && first->rows.size() == 2 &&
               std::get<std::int64_t>(first->rows[0].values[0]) == 2 &&
               std::get<std::int64_t>(first->rows[1].values[0]) == 3,
           "limit skips its offset across input batches");
    expect(second && second->rows.size() == 1 &&
               std::get<std::int64_t>(second->rows[0].values[0]) == 4,
           "limit emits no more than the requested row count");
    expect(!next_batch(limit),
           "limit stops without consuming rows beyond its bound");

    std::vector<nessodb::storage::Row> zero_rows{
        {{std::int64_t{1}}},
    };
    auto zero_source =
        std::make_unique<nessodb::execution::VectorScanOperator>(
            std::move(zero_rows));
    nessodb::execution::LimitOperator zero_limit{
        std::move(zero_source), 0};
    expect(!next_batch(zero_limit), "zero limit produces no rows");
}

void test_distinct_operator() {
    std::vector<nessodb::storage::Row> rows{
        {{std::int64_t{1}, std::string{"a"}}},
        {{std::int64_t{1}, std::string{"a"}}},
        {{nessodb::types::NullValue{}, std::string{"b"}}},
        {{std::int64_t{2}, std::string{"a"}}},
        {{nessodb::types::NullValue{}, std::string{"b"}}},
    };
    auto source = std::make_unique<nessodb::execution::VectorScanOperator>(
        std::move(rows), 1);
    nessodb::execution::DistinctOperator distinct{
        std::move(source), 2};

    const auto first = next_batch(distinct);
    const auto second = next_batch(distinct);
    expect(first && first->rows.size() == 2 &&
               std::get<std::int64_t>(first->rows[0].values[0]) == 1 &&
               std::holds_alternative<nessodb::types::NullValue>(
                   first->rows[1].values[0]),
           "distinct removes duplicates across input batches");
    expect(second && second->rows.size() == 1 &&
               std::get<std::int64_t>(second->rows[0].values[0]) == 2,
           "distinct preserves the first occurrence of each row");
    expect(!next_batch(distinct),
           "distinct signals exhaustion after unique rows");

    std::vector<nessodb::storage::Row> limited_rows{
        {{std::int64_t{1}}},
    };
    auto limited_source =
        std::make_unique<nessodb::execution::VectorScanOperator>(
            std::move(limited_rows));
    nessodb::execution::DistinctOperator limited{
        std::move(limited_source), nessodb::execution::default_batch_size, 1};
    const auto limited_result = limited.next();
    expect(!limited_result &&
               limited_result.error().code ==
                   nessodb::execution::OperatorErrorCode::memory_limit_exceeded,
           "distinct reports an exhausted memory budget");
}

void test_sort_operator() {
    std::vector<nessodb::storage::Row> rows{
        {{std::int64_t{2}, std::string{"b"}}},
        {{std::int64_t{1}, std::string{"c"}}},
        {{std::int64_t{2}, std::string{"a"}}},
        {{nessodb::types::NullValue{}, std::string{"z"}}},
    };
    auto source = std::make_unique<nessodb::execution::VectorScanOperator>(
        std::move(rows), 1);
    nessodb::execution::SortOperator sort{
        std::move(source),
        {{0, nessodb::execution::SortDirection::ascending},
         {1, nessodb::execution::SortDirection::descending}},
        2};

    const auto first = next_batch(sort);
    const auto second = next_batch(sort);
    expect(first && first->rows.size() == 2 &&
               std::get<std::int64_t>(first->rows[0].values[0]) == 1 &&
               std::get<std::string>(first->rows[1].values[1]) == "b",
           "sort orders by multiple keys before emitting its first batch");
    expect(second && second->rows.size() == 2 &&
               std::get<std::string>(second->rows[0].values[1]) == "a" &&
               std::holds_alternative<nessodb::types::NullValue>(
                   second->rows[1].values[0]),
           "ascending sort places NULL after non-NULL values");
    expect(!next_batch(sort),
           "sort signals exhaustion after ordered batches");

    std::vector<nessodb::storage::Row> descending_rows{
        {{std::int64_t{1}}},
        {{nessodb::types::NullValue{}}},
    };
    auto descending_source =
        std::make_unique<nessodb::execution::VectorScanOperator>(
            std::move(descending_rows));
    nessodb::execution::SortOperator descending{
        std::move(descending_source),
        {{0, nessodb::execution::SortDirection::descending}}};
    const auto descending_output = next_batch(descending);
    expect(descending_output &&
               std::holds_alternative<nessodb::types::NullValue>(
                   descending_output->rows.front().values.front()),
           "descending sort places NULL before non-NULL values");

    std::vector<nessodb::storage::Row> limited_rows{
        {{std::int64_t{1}}},
    };
    auto limited_source =
        std::make_unique<nessodb::execution::VectorScanOperator>(
            std::move(limited_rows));
    nessodb::execution::SortOperator limited{
        std::move(limited_source),
        {{0, nessodb::execution::SortDirection::ascending}},
        nessodb::execution::default_batch_size, 1};
    const auto limited_result = limited.next();
    expect(!limited_result &&
               limited_result.error().code ==
                   nessodb::execution::OperatorErrorCode::memory_limit_exceeded,
           "sort reports an exhausted memory budget");
}

}  // namespace

int main() {
    test_filter_and_projection();
    test_wildcard_and_literal_selects();
    test_vector_scan_batches();
    test_filter_operator();
    test_projection_operator();
    test_limit_operator();
    test_distinct_operator();
    test_sort_operator();

    if (failures != 0) {
        std::cerr << failures << " SELECT executor assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
