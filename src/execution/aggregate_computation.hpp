#pragma once

#include "binder/bound_expression.hpp"
#include "execution/aggregate_state.hpp"

#include <memory>
#include <vector>

namespace minidb::execution {

struct AggregateComputation {
    std::unique_ptr<AggregateState> state;
    std::vector<binder::BoundExpression> arguments;
    sql::SourceLocation location;
};

}  // namespace minidb::execution
