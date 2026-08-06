#pragma once

#include "sql/token.hpp"

#include <memory>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace minidb::sql {

enum class LiteralType {
    integer,
    string,
    null,
};

struct LiteralExpression {
    LiteralType type;
    std::string_view text;
    SourceLocation location;
};

struct ColumnReferenceExpression {
    std::string_view name;
    SourceLocation location;
};

using SelectExpression =
    std::variant<LiteralExpression, ColumnReferenceExpression>;

struct TableReference {
    std::string_view name;
    SourceLocation location;
};

enum class ComparisonOperator {
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
};

struct ComparisonPredicate {
    ColumnReferenceExpression column;
    ComparisonOperator comparison;
    LiteralExpression value;
};

struct NullPredicate {
    ColumnReferenceExpression column;
    bool negated{};
};

struct LogicalPredicate;
struct NegationPredicate;

using Predicate =
    std::variant<ComparisonPredicate, NullPredicate,
                 std::unique_ptr<LogicalPredicate>,
                 std::unique_ptr<NegationPredicate>>;

enum class LogicalOperator {
    conjunction,
    disjunction,
};

struct LogicalPredicate {
    LogicalOperator operation;
    Predicate left;
    Predicate right;
};

struct NegationPredicate {
    Predicate operand;
};

enum class OrderDirection {
    ascending,
    descending,
};

struct OrderByTerm {
    ColumnReferenceExpression column;
    OrderDirection direction;
};

struct LimitClause {
    std::string_view count;
    SourceLocation location;
};

struct OffsetClause {
    std::string_view count;
    SourceLocation location;
};

struct SelectStatement {
    std::vector<SelectExpression> expressions;
    bool select_all_columns{};
    bool distinct{};
    std::optional<TableReference> from;
    std::optional<Predicate> where;
    std::vector<OrderByTerm> order_by;
    std::optional<LimitClause> limit;
    std::optional<OffsetClause> offset;
};

enum class ColumnType {
    integer,
    text,
};

struct ColumnDefinition {
    std::string_view name;
    ColumnType type;
    SourceLocation location;
};

struct CreateTableStatement {
    std::string_view table_name;
    SourceLocation table_location;
    std::vector<ColumnDefinition> columns;
};

struct InsertStatement {
    std::string_view table_name;
    SourceLocation table_location;
    std::vector<LiteralExpression> values;
};

struct DeleteStatement {
    TableReference from;
    std::optional<Predicate> where;
};

struct UpdateAssignment {
    ColumnReferenceExpression column;
    LiteralExpression value;
};

struct UpdateStatement {
    TableReference table;
    std::vector<UpdateAssignment> assignments;
    std::optional<Predicate> where;
};

using Statement =
    std::variant<SelectStatement, CreateTableStatement, InsertStatement,
                 DeleteStatement, UpdateStatement>;

}  // namespace minidb::sql
