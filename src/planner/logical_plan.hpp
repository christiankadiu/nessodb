#pragma once

#include "binder/bound_statement.hpp"
#include "common/table_id.hpp"

#include <cstddef>
#include <memory>
#include <variant>
#include <vector>

namespace minidb::planner {

struct LogicalPlan;
using LogicalPlanPtr = std::unique_ptr<LogicalPlan>;

struct LogicalTableScan {
    common::TableId table_id;
};

struct LogicalOneRow {};

struct LogicalFilter {
    binder::BoundExpression predicate;
    LogicalPlanPtr child;
};

struct LogicalSort {
    std::vector<binder::BoundOrderByTerm> terms;
    LogicalPlanPtr child;
};

struct LogicalProjection {
    std::vector<binder::BoundExpression> expressions;
    LogicalPlanPtr child;
};

struct LogicalAggregate {
    std::vector<binder::BoundAggregateExpression> aggregates;
    LogicalPlanPtr child;
};

struct LogicalDistinct {
    LogicalPlanPtr child;
};

struct LogicalLimit {
    std::size_t limit;
    std::size_t offset;
    LogicalPlanPtr child;
};

using LogicalPlanNode =
    std::variant<LogicalTableScan, LogicalOneRow, LogicalFilter, LogicalSort,
                 LogicalProjection, LogicalAggregate, LogicalDistinct,
                 LogicalLimit>;

struct LogicalPlan {
    LogicalPlanNode node;
};

}  // namespace minidb::planner
