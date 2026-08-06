#pragma once

#include "execution/operator.hpp"
#include "types/value.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

namespace minidb::execution {

inline constexpr std::size_t default_distinct_memory_limit =
    64 * 1024 * 1024;

class DistinctOperator final : public RowOperator {
public:
    DistinctOperator(
        std::unique_ptr<RowOperator> child,
        std::size_t batch_size = default_batch_size,
        std::size_t memory_limit = default_distinct_memory_limit);

    [[nodiscard]] OperatorResult next() override;

private:
    struct RowHash {
        [[nodiscard]] std::size_t operator()(
            const std::vector<types::Value>& values) const noexcept;
    };

    std::unique_ptr<RowOperator> child_;
    std::size_t batch_size_;
    std::size_t memory_limit_;
    std::size_t memory_used_{};
    std::unordered_set<std::vector<types::Value>, RowHash> seen_;
    std::optional<RowBatch> input_batch_;
    std::size_t input_offset_{};
};

}  // namespace minidb::execution
