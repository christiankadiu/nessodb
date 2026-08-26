#include "planner/physical_plan_formatter.hpp"

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace minidb::planner {
namespace {

template <typename... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};

template <typename... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

std::string indentation(std::size_t depth) {
    return std::string(depth * 2, ' ');
}

void append_plan(const PhysicalPlan& plan, std::size_t depth,
                 std::vector<std::string>& lines);

void append_child(const PhysicalPlanPtr& child, std::size_t depth,
                  std::vector<std::string>& lines) {
    if (!child) {
        throw std::logic_error{"physical plan node requires a child"};
    }
    append_plan(*child, depth, lines);
}

void append_plan(const PhysicalPlan& plan, std::size_t depth,
                 std::vector<std::string>& lines) {
    std::visit(
        Overloaded{
            [depth, &lines](const PhysicalSequentialScan& scan) {
                lines.push_back(indentation(depth) +
                                "Sequential Scan (table_id=" +
                                std::to_string(scan.table_id.value) + ')');
            },
            [depth, &lines](const PhysicalOneRow&) {
                lines.push_back(indentation(depth) + "One Row");
            },
            [depth, &lines](const PhysicalFilter& filter) {
                lines.push_back(indentation(depth) + "Filter");
                append_child(filter.child, depth + 1, lines);
            },
            [depth, &lines](const PhysicalNestedLoopJoin& join) {
                lines.push_back(indentation(depth) + "Nested Loop Join");
                append_child(join.left, depth + 1, lines);
                append_child(join.right, depth + 1, lines);
            },
            [depth, &lines](const PhysicalInMemorySort& sort) {
                std::string line = indentation(depth) +
                                   "In-Memory Sort (keys=";
                for (std::size_t index = 0; index < sort.keys.size();
                     ++index) {
                    if (index != 0) {
                        line += ", ";
                    }
                    const auto& key = sort.keys[index];
                    line += "column[" + std::to_string(key.column_index) +
                            "] ";
                    line += key.direction ==
                                    PhysicalSortDirection::ascending
                                ? "ASC"
                                : "DESC";
                }
                lines.push_back(std::move(line) + ')');
                append_child(sort.child, depth + 1, lines);
            },
            [depth, &lines](const PhysicalProjection& projection) {
                lines.push_back(
                    indentation(depth) + "Projection (expressions=" +
                    std::to_string(projection.expressions.size()) + ')');
                append_child(projection.child, depth + 1, lines);
            },
            [depth, &lines](const PhysicalGlobalAggregate& aggregate) {
                lines.push_back(
                    indentation(depth) + "Global Aggregate (aggregates=" +
                    std::to_string(aggregate.aggregates.size()) + ')');
                append_child(aggregate.child, depth + 1, lines);
            },
            [depth, &lines](const PhysicalHashAggregate& aggregate) {
                lines.push_back(
                    indentation(depth) + "Hash Aggregate (keys=" +
                    std::to_string(aggregate.group_keys.size()) +
                    ", aggregates=" +
                    std::to_string(aggregate.aggregates.size()) + ')');
                append_child(aggregate.child, depth + 1, lines);
            },
            [depth, &lines](const PhysicalHashDistinct& distinct) {
                lines.push_back(indentation(depth) + "Hash Distinct");
                append_child(distinct.child, depth + 1, lines);
            },
            [depth, &lines](const PhysicalLimit& limit) {
                std::string count =
                    limit.limit == std::numeric_limits<std::size_t>::max()
                        ? "unbounded"
                        : std::to_string(limit.limit);
                lines.push_back(indentation(depth) + "Limit (limit=" +
                                count + ", offset=" +
                                std::to_string(limit.offset) + ')');
                append_child(limit.child, depth + 1, lines);
            }},
        plan.node);
}

}  // namespace

std::vector<std::string> format_physical_plan(const PhysicalPlan& plan) {
    std::vector<std::string> lines;
    append_plan(plan, 0, lines);
    return lines;
}

}  // namespace minidb::planner
