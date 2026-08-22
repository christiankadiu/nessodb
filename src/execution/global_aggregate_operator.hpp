#pragma once

#include "binder/bound_expression.hpp"
#include "execution/aggregate_state.hpp"
#include "execution/operator.hpp"

#include <memory>
#include <vector>

namespace minidb::execution {

struct AggregateComputation {
    std::unique_ptr<AggregateState> state;
    std::vector<binder::BoundExpression> arguments;
    sql::SourceLocation location;
};

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
