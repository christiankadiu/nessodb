#pragma once

#include "execution/operator.hpp"

#include <cstddef>
#include <memory>

namespace nessodb::execution {

class LimitOperator final : public RowOperator {
public:
    LimitOperator(
        std::unique_ptr<RowOperator> child,
        std::size_t limit,
        std::size_t offset = 0);

    [[nodiscard]] OperatorResult next() override;

private:
    std::unique_ptr<RowOperator> child_;
    std::size_t limit_;
    std::size_t offset_;
    std::size_t skipped_{0};
    std::size_t emitted_{0};
};

}  // namespace nessodb::execution
