#pragma once

#include "binder/bound_expression.hpp"
#include "execution/aggregate_computation.hpp"
#include "execution/operator.hpp"
#include "types/value.hpp"

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

namespace nessodb::execution {

inline constexpr std::size_t default_hash_aggregate_memory_limit =
    64 * 1024 * 1024;

class HashAggregateOperator final : public RowOperator {
public:
    HashAggregateOperator(
        std::unique_ptr<RowOperator> child,
        std::vector<binder::BoundExpression> group_keys,
        std::vector<AggregateComputation> aggregates,
        std::size_t batch_size = default_batch_size,
        std::size_t memory_limit = default_hash_aggregate_memory_limit);

    [[nodiscard]] OperatorResult next() override;

private:
    struct ValueVectorHash {
        [[nodiscard]] std::size_t operator()(
            const std::vector<types::Value>& values) const noexcept;
    };

    struct Group {
        std::vector<types::Value> keys;
        std::vector<std::unique_ptr<AggregateState>> states;
    };

    [[nodiscard]] std::expected<void, OperatorError> materialize();

    std::unique_ptr<RowOperator> child_;
    std::vector<binder::BoundExpression> group_keys_;
    std::vector<AggregateComputation> aggregates_;
    std::size_t batch_size_;
    std::size_t memory_limit_;
    std::size_t memory_used_{};
    std::unordered_map<std::vector<types::Value>, std::size_t,
                       ValueVectorHash>
        group_indexes_;
    std::vector<Group> groups_;
    std::size_t output_offset_{};
    bool materialized_{};
};

}  // namespace nessodb::execution
