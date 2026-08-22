#pragma once

#include "execution/aggregate_computation.hpp"
#include "execution/operator.hpp"

#include <memory>
#include <vector>

namespace minidb::execution {

class GlobalAggregateOperator final : public RowOperator {
public:
    GlobalAggregateOperator(
        std::unique_ptr<RowOperator> child,
        std::vector<AggregateComputation> aggregates);

    [[nodiscard]] OperatorResult next() override;

private:
    std::unique_ptr<RowOperator> child_;
    std::vector<AggregateComputation> aggregates_;
    bool finished_{};
};

}  // namespace minidb::execution
