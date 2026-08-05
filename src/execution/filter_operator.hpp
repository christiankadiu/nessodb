#pragma once

#include "binder/bound_statement.hpp"
#include "execution/operator.hpp"

#include <memory>
#include <optional>

namespace minidb::execution {

class FilterOperator final : public RowOperator {
public:
    FilterOperator(std::unique_ptr<RowOperator> child,
                   const binder::BoundPredicate& predicate);

    [[nodiscard]] std::optional<RowBatch> next() override;

private:
    std::unique_ptr<RowOperator> child_;
    const binder::BoundPredicate& predicate_;
};

}  // namespace minidb::execution
