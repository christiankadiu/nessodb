#pragma once

#include "execution/operator.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace minidb::execution {

inline constexpr std::size_t default_sort_memory_limit = 64 * 1024 * 1024;

enum class SortDirection {
    ascending,
    descending,
};

struct SortKey {
    std::size_t column_index;
    SortDirection direction;
};

class SortOperator final : public RowOperator {
public:
    SortOperator(std::unique_ptr<RowOperator> child,
                 std::vector<SortKey> keys,
                 std::size_t batch_size = default_batch_size,
                 std::size_t memory_limit = default_sort_memory_limit);

    [[nodiscard]] OperatorResult next() override;

private:
    [[nodiscard]] std::expected<void, OperatorError> materialize();

    std::unique_ptr<RowOperator> child_;
    std::vector<SortKey> keys_;
    std::size_t batch_size_;
    std::size_t memory_limit_;
    std::vector<storage::Row> rows_;
    std::size_t offset_{};
    bool materialized_{};
};

}  // namespace minidb::execution
