#pragma once

#include "binder/bound_statement.hpp"
#include "common/table_id.hpp"

#include <cstddef>
#include <memory>
#include <variant>
#include <vector>

namespace minidb::planner {

struct PhysicalPlan;
using PhysicalPlanPtr = std::unique_ptr<PhysicalPlan>;

struct PhysicalSequentialScan {
    common::TableId table_id;
};

struct PhysicalIndexScan {
    common::TableId table_id;
    std::size_t column_index;
    types::Value key;
};

struct PhysicalOneRow {};

struct PhysicalFilter {
    binder::BoundExpression predicate;
    PhysicalPlanPtr child;
};

struct PhysicalNestedLoopJoin {
    binder::BoundExpression predicate;
    PhysicalPlanPtr left;
    PhysicalPlanPtr right;
};

enum class PhysicalSortDirection {
    ascending,
    descending,
};

struct PhysicalSortKey {
    std::size_t column_index;
    PhysicalSortDirection direction;
};

struct PhysicalInMemorySort {
    std::vector<PhysicalSortKey> keys;
    PhysicalPlanPtr child;
};

struct PhysicalProjection {
    std::vector<binder::BoundExpression> expressions;
    PhysicalPlanPtr child;
};

struct PhysicalGlobalAggregate {
    std::vector<binder::BoundAggregateExpression> aggregates;
    PhysicalPlanPtr child;
};

struct PhysicalHashAggregate {
    std::vector<binder::BoundExpression> group_keys;
    std::vector<binder::BoundAggregateExpression> aggregates;
    PhysicalPlanPtr child;
};

struct PhysicalHashDistinct {
    PhysicalPlanPtr child;
};

struct PhysicalLimit {
    std::size_t limit;
    std::size_t offset;
    PhysicalPlanPtr child;
};

using PhysicalPlanNode =
    std::variant<PhysicalSequentialScan, PhysicalIndexScan, PhysicalOneRow,
                 PhysicalFilter, PhysicalNestedLoopJoin,
                 PhysicalInMemorySort, PhysicalProjection,
                 PhysicalGlobalAggregate, PhysicalHashAggregate,
                 PhysicalHashDistinct, PhysicalLimit>;

struct PhysicalPlan {
    PhysicalPlanNode node;
};

}  // namespace minidb::planner
