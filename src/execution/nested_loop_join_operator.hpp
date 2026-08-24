#pragma once

#include "binder/bound_expression.hpp"
#include "execution/operator.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace minidb::execution {

inline constexpr std::size_t default_nested_loop_join_memory_limit =
    64 * 1024 * 1024;

class NestedLoopJoinOperator final : public RowOperator {
public:
    NestedLoopJoinOperator(
        std::unique_ptr<RowOperator> left,
        std::unique_ptr<RowOperator> right,
        std::optional<binder::BoundExpression> predicate = std::nullopt,
        std::size_t batch_size = default_batch_size,
        std::size_t memory_limit = default_nested_loop_join_memory_limit);

    [[nodiscard]] OperatorResult next() override;

private:
    [[nodiscard]] std::expected<void, OperatorError> materialize_right();

    std::unique_ptr<RowOperator> left_;
    std::unique_ptr<RowOperator> right_;
    std::optional<binder::BoundExpression> predicate_;
    std::size_t batch_size_;
    std::size_t memory_limit_;
    std::vector<storage::Row> right_rows_;
    std::optional<RowBatch> left_batch_;
    std::size_t left_offset_{};
    std::size_t right_offset_{};
    bool right_materialized_{};
};

}  // namespace minidb::execution
