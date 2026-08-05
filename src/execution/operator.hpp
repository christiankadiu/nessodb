#pragma once

#include "storage/access/row.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace minidb::execution {

inline constexpr std::size_t default_batch_size = 1024;

struct RowBatch {
    std::vector<storage::Row> rows;
};

class RowOperator {
public:
    virtual ~RowOperator() = default;

    [[nodiscard]] virtual std::optional<RowBatch> next() = 0;
};

}  // namespace minidb::execution
