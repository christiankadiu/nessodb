#include "execution/nested_loop_join_operator.hpp"
#include "execution/vector_scan_operator.hpp"

#include "binder/bound_expression.hpp"
#include "storage/access/row.hpp"
#include "types/value.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using nessodb::binder::BoundColumnReferenceExpression;
using nessodb::binder::BoundComparisonExpression;
using nessodb::binder::BoundComparisonOperator;
using nessodb::binder::BoundExpression;
using nessodb::execution::NestedLoopJoinOperator;
using nessodb::execution::OperatorErrorCode;
using nessodb::execution::VectorScanOperator;

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

BoundExpression equality(std::size_t left_index,
                         std::size_t right_index) {
    return BoundExpression{BoundComparisonExpression{
        BoundComparisonOperator::equal,
        std::make_unique<BoundExpression>(
            BoundColumnReferenceExpression{left_index, {}}),
        std::make_unique<BoundExpression>(
            BoundColumnReferenceExpression{right_index, {}})}};
}

std::vector<nessodb::storage::Row> collect(
    nessodb::execution::RowOperator& source) {
    std::vector<nessodb::storage::Row> rows;
    while (true) {
        auto batch = source.next();
        expect(batch.has_value(), "join executes without runtime error");
        if (!batch || !*batch) {
            return rows;
        }
        for (auto& row : (*batch)->rows) {
            rows.push_back(std::move(row));
        }
    }
}

void test_cross_product_order_and_batches() {
    auto left = std::make_unique<VectorScanOperator>(
        std::vector<nessodb::storage::Row>{
            {{std::int64_t{1}}}, {{std::int64_t{2}}}},
        1);
    auto right = std::make_unique<VectorScanOperator>(
        std::vector<nessodb::storage::Row>{
            {{std::string{"a"}}}, {{std::string{"b"}}}},
        1);
    NestedLoopJoinOperator join{
        std::move(left), std::move(right), std::nullopt, 3};

    const auto first = join.next();
    const auto second = join.next();
    const auto end = join.next();
    expect(first && *first && (*first)->rows.size() == 3 &&
               second && *second && (*second)->rows.size() == 1,
           "cross product respects the configured output batch size");
    if (first && *first && (*first)->rows.size() == 3 &&
        second && *second && (*second)->rows.size() == 1) {
        expect(std::get<std::int64_t>((*first)->rows[0].values[0]) == 1 &&
                   std::get<std::string>((*first)->rows[0].values[1]) == "a" &&
                   std::get<std::string>((*first)->rows[1].values[1]) == "b" &&
                   std::get<std::int64_t>((*first)->rows[2].values[0]) == 2 &&
                   std::get<std::string>((*second)->rows[0].values[1]) == "b",
               "nested loop output is deterministic and left-major");
    }
    expect(end && !*end, "cross product signals exhaustion");
}

void test_inner_join_predicate() {
    auto left = std::make_unique<VectorScanOperator>(
        std::vector<nessodb::storage::Row>{
            {{std::int64_t{1}}},
            {{std::int64_t{2}}},
            {{std::int64_t{3}}}},
        2);
    auto right = std::make_unique<VectorScanOperator>(
        std::vector<nessodb::storage::Row>{
            {{std::int64_t{2}, std::string{"two"}}},
            {{std::int64_t{3}, std::string{"three"}}}},
        1);
    NestedLoopJoinOperator join{
        std::move(left), std::move(right), equality(0, 1), 1};

    const auto rows = collect(join);
    expect(rows.size() == 2 && rows[0].values.size() == 3,
           "inner join emits only matching combined rows");
    if (rows.size() == 2) {
        expect(std::get<std::int64_t>(rows[0].values[0]) == 2 &&
                   std::get<std::string>(rows[0].values[2]) == "two" &&
                   std::get<std::int64_t>(rows[1].values[0]) == 3 &&
                   std::get<std::string>(rows[1].values[2]) == "three",
               "join predicate sees left and right column offsets");
    }
}

void test_empty_right_and_memory_limit() {
    auto left = std::make_unique<VectorScanOperator>(
        std::vector<nessodb::storage::Row>{{{std::int64_t{1}}}});
    auto empty_right = std::make_unique<VectorScanOperator>(
        std::vector<nessodb::storage::Row>{});
    NestedLoopJoinOperator empty{
        std::move(left), std::move(empty_right)};
    const auto empty_result = empty.next();
    expect(empty_result && !*empty_result,
           "an empty right input produces an empty inner join");

    auto limited_left = std::make_unique<VectorScanOperator>(
        std::vector<nessodb::storage::Row>{{{std::int64_t{1}}}});
    auto limited_right = std::make_unique<VectorScanOperator>(
        std::vector<nessodb::storage::Row>{{{std::string{"value"}}}});
    NestedLoopJoinOperator limited{
        std::move(limited_left), std::move(limited_right), std::nullopt,
        nessodb::execution::default_batch_size, 1};
    const auto limited_result = limited.next();
    expect(!limited_result && limited_result.error().code ==
                                  OperatorErrorCode::memory_limit_exceeded,
           "right-side materialization enforces its memory budget");
}

void test_invalid_configuration() {
    bool rejected = false;
    try {
        auto left = std::make_unique<VectorScanOperator>(
            std::vector<nessodb::storage::Row>{});
        auto right = std::make_unique<VectorScanOperator>(
            std::vector<nessodb::storage::Row>{});
        NestedLoopJoinOperator invalid{
            std::move(left), std::move(right),
            BoundExpression{BoundColumnReferenceExpression{0, {}}}};
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    expect(rejected, "nested loop join requires a boolean predicate");
}

}  // namespace

int main() {
    test_cross_product_order_and_batches();
    test_inner_join_predicate();
    test_empty_right_and_memory_limit();
    test_invalid_configuration();

    if (failures != 0) {
        std::cerr << failures
                  << " nested loop join operator assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
