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

struct ColumnQualifier {
    std::string_view name;
    SourceLocation location;
};

struct ColumnReferenceExpression {
    std::string_view name;
    SourceLocation location;
    std::optional<ColumnQualifier> qualifier{};
};

enum class UnaryArithmeticOperator {
    plus,
    minus,
};

enum class BinaryArithmeticOperator {
    addition,
    subtraction,
    multiplication,
    division,
};

enum class ComparisonOperator {
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
};

enum class LogicalOperator {
    conjunction,
    disjunction,
};

struct Expression;
using ExpressionPtr = std::unique_ptr<Expression>;

struct UnaryArithmeticExpression {
    UnaryArithmeticOperator operation;
    ExpressionPtr operand;
    SourceLocation location;
};

struct BinaryArithmeticExpression {
    BinaryArithmeticOperator operation;
    ExpressionPtr left;
    ExpressionPtr right;
    SourceLocation location;
};

struct ComparisonExpression {
    ComparisonOperator comparison;
    ExpressionPtr left;
    ExpressionPtr right;
    SourceLocation location;
};

struct NullTestExpression {
    ExpressionPtr operand;
    bool negated{};
    SourceLocation location;
};

struct LogicalExpression {
    LogicalOperator operation;
    ExpressionPtr left;
    ExpressionPtr right;
    SourceLocation location;
};

struct NegationExpression {
    ExpressionPtr operand;
    SourceLocation location;
};

struct FunctionCallExpression {
    std::string_view name;
    std::vector<ExpressionPtr> arguments;
    bool star_argument{};
    SourceLocation location;
};

using ExpressionNode = std::variant<
    LiteralExpression, ColumnReferenceExpression,
    UnaryArithmeticExpression, BinaryArithmeticExpression,
    ComparisonExpression, NullTestExpression, LogicalExpression,
    NegationExpression, FunctionCallExpression>;

struct ExpressionAlias {
    std::string_view name;
    SourceLocation location;
};

struct Expression {
    Expression(LiteralExpression expression)
        : node(std::move(expression)) {}
    Expression(ColumnReferenceExpression expression)
        : node(std::move(expression)) {}
    Expression(UnaryArithmeticExpression expression)
        : node(std::move(expression)) {}
    Expression(BinaryArithmeticExpression expression)
        : node(std::move(expression)) {}
    Expression(ComparisonExpression expression)
        : node(std::move(expression)) {}
    Expression(NullTestExpression expression)
        : node(std::move(expression)) {}
    Expression(LogicalExpression expression)
        : node(std::move(expression)) {}
    Expression(NegationExpression expression)
        : node(std::move(expression)) {}
    Expression(FunctionCallExpression expression)
        : node(std::move(expression)) {}

    ExpressionNode node;
    std::optional<ExpressionAlias> alias;
};

struct TableAlias {
    std::string_view name;
    SourceLocation location;
};

struct TableReference {
    std::string_view name;
    SourceLocation location;
    std::optional<TableAlias> alias{};
};

struct JoinClause {
    TableReference table;
    Expression condition;
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
    std::vector<Expression> expressions;
    bool select_all_columns{};
    bool distinct{};
    std::optional<TableReference> from;
    std::vector<JoinClause> joins;
    std::optional<Expression> where;
    std::vector<Expression> group_by;
    std::vector<OrderByTerm> order_by;
    std::optional<LimitClause> limit;
    std::optional<OffsetClause> offset;
};

struct ExplainStatement {
    SelectStatement select;
};

enum class ColumnType {
    integer,
    text,
};

struct ColumnDefinition {
    std::string_view name;
    ColumnType type;
    SourceLocation location;
    bool primary_key{};
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
    std::optional<Expression> where;
};

struct UpdateAssignment {
    ColumnReferenceExpression column;
    LiteralExpression value;
};

struct UpdateStatement {
    TableReference table;
    std::vector<UpdateAssignment> assignments;
    std::optional<Expression> where;
};

struct BeginStatement {};
struct CommitStatement {};
struct RollbackStatement {};

using Statement =
    std::variant<SelectStatement, ExplainStatement, CreateTableStatement,
                 InsertStatement, DeleteStatement, UpdateStatement,
                 BeginStatement, CommitStatement, RollbackStatement>;

}  // namespace minidb::sql
