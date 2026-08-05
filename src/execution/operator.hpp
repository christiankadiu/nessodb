#pragma once

#include "storage/access/row.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <vector>

namespace minidb::execution {

inline constexpr std::size_t default_batch_size = 1024;

struct RowBatch {
    std::vector<storage::Row> rows;
};

enum class OperatorErrorCode {
    memory_limit_exceeded,
};

struct OperatorError {
    OperatorErrorCode code;
};

using OperatorResult =
    std::expected<std::optional<RowBatch>, OperatorError>;

class RowOperator {
public:
    virtual ~RowOperator() = default;

    [[nodiscard]] virtual OperatorResult next() = 0;
};

}  // namespace minidb::execution
