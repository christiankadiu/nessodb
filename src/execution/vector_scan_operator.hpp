#pragma once

#include "execution/operator.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace minidb::execution {

class VectorScanOperator final : public RowOperator {
public:
    explicit VectorScanOperator(
        std::vector<storage::Row> rows,
        std::size_t batch_size = default_batch_size);

    [[nodiscard]] std::optional<RowBatch> next() override;

private:
    std::vector<storage::Row> rows_;
    std::size_t batch_size_;
    std::size_t offset_{};
};

}  // namespace minidb::execution
