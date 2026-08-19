#pragma once

#include "binder/bound_statement.hpp"
#include "execution/operator.hpp"

#include <memory>
#include <optional>

namespace minidb::execution {

class FilterOperator final : public RowOperator {
public:
    FilterOperator(std::unique_ptr<RowOperator> child,
                   binder::BoundExpression predicate);

    [[nodiscard]] OperatorResult next() override;

private:
    std::unique_ptr<RowOperator> child_;
    binder::BoundExpression predicate_;
};

}  // namespace minidb::execution
