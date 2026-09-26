#pragma once

#include "binder/bound_statement.hpp"
#include "execution/operator.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace nessodb::execution {

class ProjectionOperator final : public RowOperator {
public:
    ProjectionOperator(
        std::unique_ptr<RowOperator> child,
        std::vector<binder::BoundExpression> expressions);

    [[nodiscard]] OperatorResult next() override;

private:
    std::unique_ptr<RowOperator> child_;
    std::vector<binder::BoundExpression> expressions_;
};

}  // namespace nessodb::execution
