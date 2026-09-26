#include "sql/parser.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <variant>

namespace {

using nessodb::sql::ParseErrorCode;
using nessodb::sql::Parser;
using nessodb::sql::ColumnReferenceExpression;
using nessodb::sql::ColumnType;
using nessodb::sql::BinaryArithmeticExpression;
using nessodb::sql::BinaryArithmeticOperator;
using nessodb::sql::ComparisonOperator;
using nessodb::sql::ComparisonExpression;
using nessodb::sql::LiteralExpression;
using nessodb::sql::LiteralType;
using nessodb::sql::LogicalOperator;
using nessodb::sql::LogicalExpression;
using nessodb::sql::NegationExpression;
using nessodb::sql::NullTestExpression;
using nessodb::sql::SourceLocation;
using nessodb::sql::UnaryArithmeticExpression;
using nessodb::sql::UnaryArithmeticOperator;

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

void test_select_list() {
    Parser parser{"SELECT 1, 'Alice', NULL;"};
    const auto result = parser.parse_select_statement();

    expect(result.has_value(), "valid SELECT is parsed");
    if (!result) {
        return;
    }

    expect(result->expressions.size() == 3, "SELECT contains three expressions");
    if (result->expressions.size() == 3) {
        const auto& first =
            std::get<LiteralExpression>(result->expressions[0].node);
        const auto& second =
            std::get<LiteralExpression>(result->expressions[1].node);
        const auto& third =
            std::get<LiteralExpression>(result->expressions[2].node);
        expect(first.type == LiteralType::integer,
               "first expression is an integer");
        expect(first.text == "1", "integer text is preserved");
        expect(first.location == SourceLocation{7, 1, 8},
               "first integer location is preserved");
        expect(second.type == LiteralType::string,
               "second expression is a string");
        expect(second.text == "'Alice'", "string text is preserved");
        expect(third.type == LiteralType::null,
               "third expression is NULL");
        expect(third.text == "NULL", "NULL text is preserved");
    }
}

void test_select_columns() {
    Parser parser{"SELECT name, id, 'active' FROM users;"};
    const auto result = parser.parse_select_statement();

    expect(result.has_value(), "column projection is parsed");
    if (!result) {
        return;
    }
    expect(result->expressions.size() == 3,
           "column projection preserves every expression");
    if (result->expressions.size() == 3) {
        const auto& name =
            std::get<ColumnReferenceExpression>(result->expressions[0].node);
        const auto& id =
            std::get<ColumnReferenceExpression>(result->expressions[1].node);
        expect(name.name == "name" && id.name == "id",
               "column names preserve projection order");
        expect(std::holds_alternative<LiteralExpression>(
                   result->expressions[2].node),
               "projection can contain a literal");
    }
    expect(result->from && result->from->name == "users",
           "column projection preserves its table");
}

void test_select_where() {
    Parser parser{"SELECT name FROM users WHERE id = 7;"};
    const auto result = parser.parse_select_statement();

    expect(result.has_value(), "WHERE equality predicate is parsed");
    if (!result || !result->where ||
        !std::holds_alternative<ComparisonExpression>(result->where->node)) {
        return;
    }
    const auto& predicate = std::get<ComparisonExpression>(result->where->node);
    expect(predicate.left &&
               std::get<ColumnReferenceExpression>(predicate.left->node).name ==
                   "id",
           "WHERE column is preserved");
    expect(predicate.comparison == ComparisonOperator::equal,
           "WHERE comparison operator is preserved");
    expect(predicate.right &&
               std::get<LiteralExpression>(predicate.right->node).type ==
                   LiteralType::integer &&
               std::get<LiteralExpression>(predicate.right->node).text == "7",
           "WHERE literal is preserved");
}

void test_select_null_predicates() {
    Parser null_parser{"SELECT id FROM users WHERE name IS NULL;"};
    const auto null_result = null_parser.parse_select_statement();
    expect(null_result && null_result->where &&
               std::holds_alternative<NullTestExpression>(null_result->where->node),
           "IS NULL predicate is parsed");
    if (null_result && null_result->where &&
        std::holds_alternative<NullTestExpression>(null_result->where->node)) {
        const auto& predicate = std::get<NullTestExpression>(null_result->where->node);
        expect(predicate.operand &&
                   std::get<ColumnReferenceExpression>(
                       predicate.operand->node).name == "name" &&
                   !predicate.negated,
               "IS NULL preserves column and polarity");
    }

    Parser not_null_parser{"SELECT id FROM users WHERE name IS NOT NULL;"};
    const auto not_null_result = not_null_parser.parse_select_statement();
    const auto* not_null_predicate =
        not_null_result && not_null_result->where
            ? std::get_if<NullTestExpression>(&not_null_result->where->node)
            : nullptr;
    expect(not_null_predicate && not_null_predicate->negated,
           "IS NOT NULL predicate is parsed as negated");
}

void test_select_and_predicate() {
    Parser parser{
        "SELECT id FROM users WHERE id >= 2 AND name IS NOT NULL;"};
    const auto result = parser.parse_select_statement();
    const auto* logical =
        result && result->where
            ? std::get_if<LogicalExpression>(&result->where->node)
            : nullptr;

    expect(logical && logical->operation == LogicalOperator::conjunction,
           "AND creates a conjunction predicate");
    if (!logical || !logical->left || !logical->right) {
        return;
    }
    expect(std::holds_alternative<ComparisonExpression>(logical->left->node),
           "AND preserves its left predicate");
    expect(std::holds_alternative<NullTestExpression>(logical->right->node),
           "AND preserves its right predicate");
}

void test_select_or_precedence() {
    Parser parser{
        "SELECT id FROM users WHERE id = 1 OR id = 2 AND name IS NOT NULL;"};
    const auto result = parser.parse_select_statement();
    const auto* disjunction =
        result && result->where
            ? std::get_if<LogicalExpression>(&result->where->node)
            : nullptr;

    expect(disjunction &&
               disjunction->operation == LogicalOperator::disjunction,
           "OR creates a disjunction predicate");
    if (!disjunction || !disjunction->right) {
        return;
    }
    const auto* conjunction =
        std::get_if<LogicalExpression>(&disjunction->right->node);
    expect(conjunction &&
               conjunction->operation == LogicalOperator::conjunction,
           "AND has higher precedence than OR");
}

void test_grouped_predicate() {
    Parser parser{
        "SELECT id FROM users WHERE (id = 1 OR id = 2) AND name IS NOT NULL;"};
    const auto result = parser.parse_select_statement();
    const auto* conjunction =
        result && result->where
            ? std::get_if<LogicalExpression>(&result->where->node)
            : nullptr;

    expect(conjunction &&
               conjunction->operation == LogicalOperator::conjunction,
           "parentheses override logical precedence");
    if (!conjunction || !conjunction->left) {
        return;
    }
    const auto* disjunction =
        std::get_if<LogicalExpression>(&conjunction->left->node);
    expect(disjunction &&
               disjunction->operation == LogicalOperator::disjunction,
           "grouped predicate preserves its nested expression");

    Parser nested_parser{
        "SELECT id FROM users WHERE ((id = 1));"};
    const auto nested = nested_parser.parse_select_statement();
    expect(nested && nested->where &&
               std::holds_alternative<ComparisonExpression>(nested->where->node),
           "predicate parentheses can be nested");

    Parser unclosed_parser{
        "SELECT id FROM users WHERE (id = 1;"};
    const auto unclosed = unclosed_parser.parse_select_statement();
    expect(!unclosed && unclosed.error().code ==
                            ParseErrorCode::expected_right_parenthesis,
           "grouped predicate requires a closing parenthesis");
}

void test_negated_predicate() {
    Parser parser{
        "SELECT id FROM users WHERE NOT id = 1 AND id = 2;"};
    const auto result = parser.parse_select_statement();
    const auto* conjunction =
        result && result->where
            ? std::get_if<LogicalExpression>(&result->where->node)
            : nullptr;

    expect(conjunction &&
               conjunction->operation == LogicalOperator::conjunction,
           "NOT has higher precedence than AND");
    if (!conjunction || !conjunction->left) {
        return;
    }
    expect(std::holds_alternative<NegationExpression>(
               conjunction->left->node),
           "NOT creates a unary predicate");

    Parser grouped_parser{
        "SELECT id FROM users WHERE NOT (id = 1 OR name IS NULL);"};
    const auto grouped = grouped_parser.parse_select_statement();
    const auto* negation =
        grouped && grouped->where
            ? std::get_if<NegationExpression>(&grouped->where->node)
            : nullptr;
    expect(negation && negation->operand &&
               std::holds_alternative<LogicalExpression>(
                   negation->operand->node),
           "NOT can negate a grouped predicate");
}

void test_optional_semicolon() {
    Parser parser{"SELECT 7"};
    const auto result = parser.parse_select_statement();

    expect(result.has_value(), "semicolon is optional");
    if (result) {
        expect(result->expressions.size() == 1, "single expression is parsed");
    }
}

void test_select_all_from_table() {
    Parser parser{"SELECT * FROM users;"};
    const auto result = parser.parse_select_statement();

    expect(result.has_value(), "wildcard table SELECT is parsed");
    if (!result) {
        return;
    }
    expect(result->select_all_columns, "wildcard projection is preserved");
    expect(result->expressions.empty(), "wildcard SELECT has no literal expressions");
    expect(result->from.has_value(), "FROM table is preserved");
    if (result->from) {
        expect(result->from->name == "users", "FROM table name is preserved");
        expect(result->from->location == SourceLocation{14, 1, 15},
               "FROM table location is preserved");
    }
}

void expect_error(std::string_view source, ParseErrorCode code, SourceLocation location,
                  std::string_view description) {
    Parser parser{source};
    const auto result = parser.parse_select_statement();

    expect(!result, description);
    if (!result) {
        expect(result.error().code == code, "parse error has expected code");
        expect(result.error().location == location, "parse error has expected location");
    }
}

void test_errors() {
    expect_error("1;", ParseErrorCode::expected_select, SourceLocation{0, 1, 1},
                 "statement must start with SELECT");
    expect_error("SELECT;", ParseErrorCode::expected_expression, SourceLocation{6, 1, 7},
                 "SELECT requires an expression");
    expect_error("SELECT 1,;", ParseErrorCode::expected_expression, SourceLocation{9, 1, 10},
                 "comma must be followed by an expression");
    expect_error("SELECT 1 2", ParseErrorCode::expected_comma_from_or_end, SourceLocation{9, 1, 10},
                 "expressions require a comma");
    expect_error("SELECT 1; 2", ParseErrorCode::expected_end_of_input, SourceLocation{10, 1, 11},
                 "tokens after semicolon are rejected");
    expect_error("SELECT @", ParseErrorCode::lexical_error, SourceLocation{7, 1, 8},
                 "lexer errors are propagated");
    expect_error("SELECT *;", ParseErrorCode::expected_from, SourceLocation{8, 1, 9},
                 "wildcard requires FROM");
    expect_error("SELECT * FROM;", ParseErrorCode::expected_identifier,
                 SourceLocation{13, 1, 14}, "FROM requires a table name");
    expect_error("SELECT * FROM users WHERE = 1;",
                 ParseErrorCode::expected_expression,
                 SourceLocation{26, 1, 27}, "WHERE requires an expression");
    expect_error("SELECT * FROM users WHERE id 1;",
                 ParseErrorCode::expected_end_of_input,
                 SourceLocation{29, 1, 30}, "WHERE requires a comparison operator");
    Parser column_comparison{
        "SELECT * FROM users WHERE id = other_id;"};
    expect(column_comparison.parse_select_statement().has_value(),
           "WHERE accepts expressions on both sides of a comparison");
    expect_error("SELECT * FROM users WHERE id IS 1;",
                 ParseErrorCode::expected_null,
                 SourceLocation{32, 1, 33}, "IS requires NULL");
    expect_error("SELECT * FROM users WHERE id IS NOT 1;",
                 ParseErrorCode::expected_null,
                 SourceLocation{36, 1, 37}, "IS NOT requires NULL");
}

void test_create_table() {
    Parser parser{"CREATE TABLE users(id INT, name TEXT);"};
    const auto result = parser.parse_create_table_statement();

    expect(result.has_value(), "valid CREATE TABLE is parsed");
    if (!result) {
        return;
    }

    expect(result->table_name == "users", "table name is preserved");
    expect(result->table_location == SourceLocation{13, 1, 14},
           "table location is preserved");
    expect(result->columns.size() == 2, "two columns are parsed");
    if (result->columns.size() == 2) {
        expect(result->columns[0].name == "id", "first column name is preserved");
        expect(result->columns[0].type == ColumnType::integer, "INT column type is parsed");
        expect(result->columns[1].name == "name", "second column name is preserved");
        expect(result->columns[1].type == ColumnType::text, "TEXT column type is parsed");
    }
}

void expect_create_error(std::string_view source, ParseErrorCode code, SourceLocation location,
                         std::string_view description) {
    Parser parser{source};
    const auto result = parser.parse_create_table_statement();

    expect(!result, description);
    if (!result) {
        expect(result.error().code == code, "CREATE TABLE error has expected code");
        expect(result.error().location == location, "CREATE TABLE error has expected location");
    }
}

void test_create_table_errors() {
    expect_create_error("TABLE users(id INT);", ParseErrorCode::expected_create,
                        SourceLocation{0, 1, 1}, "CREATE keyword is required");
    expect_create_error("CREATE users(id INT);", ParseErrorCode::expected_table,
                        SourceLocation{7, 1, 8}, "TABLE keyword is required");
    expect_create_error("CREATE TABLE (id INT);", ParseErrorCode::expected_identifier,
                        SourceLocation{13, 1, 14}, "table name is required");
    expect_create_error("CREATE TABLE users();", ParseErrorCode::expected_identifier,
                        SourceLocation{19, 1, 20}, "at least one column is required");
    expect_create_error("CREATE TABLE users(id);", ParseErrorCode::expected_column_type,
                        SourceLocation{21, 1, 22}, "column type is required");
}

void test_statement_dispatch() {
    Parser select_parser{"SELECT 1;"};
    const auto select = select_parser.parse_statement();
    expect(select.has_value(), "generic parser accepts SELECT");
    if (select) {
        expect(std::holds_alternative<nessodb::sql::SelectStatement>(*select),
               "generic parser returns a SELECT statement");
    }

    Parser explain_parser{"EXPLAIN SELECT 1;"};
    const auto explained = explain_parser.parse_statement();
    expect(explained.has_value(), "generic parser accepts EXPLAIN");
    if (explained) {
        expect(std::holds_alternative<nessodb::sql::ExplainStatement>(
                   *explained),
               "generic parser returns an EXPLAIN statement");
    }

    Parser create_parser{"CREATE TABLE users(id INT);"};
    const auto create = create_parser.parse_statement();
    expect(create.has_value(), "generic parser accepts CREATE TABLE");
    if (create) {
        expect(std::holds_alternative<nessodb::sql::CreateTableStatement>(*create),
               "generic parser returns a CREATE TABLE statement");
    }

    Parser insert_parser{"INSERT INTO users VALUES (1);"};
    const auto insert = insert_parser.parse_statement();
    expect(insert.has_value(), "generic parser accepts INSERT");
    if (insert) {
        expect(std::holds_alternative<nessodb::sql::InsertStatement>(*insert),
               "generic parser returns an INSERT statement");
    }

    Parser delete_parser{"DELETE FROM users;"};
    const auto deleted = delete_parser.parse_statement();
    expect(deleted.has_value(), "generic parser accepts DELETE");
    if (deleted) {
        expect(std::holds_alternative<nessodb::sql::DeleteStatement>(*deleted),
               "generic parser returns a DELETE statement");
    }

    Parser update_parser{"UPDATE users SET name = 'Alice';"};
    const auto updated = update_parser.parse_statement();
    expect(updated.has_value(), "generic parser accepts UPDATE");
    if (updated) {
        expect(std::holds_alternative<nessodb::sql::UpdateStatement>(*updated),
               "generic parser returns an UPDATE statement");
    }

    Parser invalid_parser{"users"};
    const auto invalid = invalid_parser.parse_statement();
    expect(!invalid, "generic parser rejects an unknown statement");
    if (!invalid) {
        expect(invalid.error().code == ParseErrorCode::expected_statement,
               "unknown statement reports the expected error");
    }
}

void test_transaction_control() {
    Parser begin_parser{"BEGIN;"};
    const auto begin = begin_parser.parse_statement();
    expect(begin && std::holds_alternative<nessodb::sql::BeginStatement>(*begin),
           "generic parser returns a BEGIN statement");

    Parser commit_parser{"commit"};
    const auto commit = commit_parser.parse_statement();
    expect(commit && std::holds_alternative<nessodb::sql::CommitStatement>(*commit),
           "generic parser returns a case-insensitive COMMIT statement");

    Parser rollback_parser{"ROLLBACK;"};
    const auto rollback = rollback_parser.parse_statement();
    expect(rollback &&
               std::holds_alternative<nessodb::sql::RollbackStatement>(*rollback),
           "generic parser returns a ROLLBACK statement");

    Parser trailing_parser{"BEGIN TRANSACTION;"};
    const auto trailing = trailing_parser.parse_begin_statement();
    expect(!trailing &&
               trailing.error().code == ParseErrorCode::expected_end_of_input &&
               trailing.error().location == SourceLocation{6, 1, 7},
           "BEGIN rejects unsupported trailing syntax");

    Parser wrong_parser{"COMMIT;"};
    const auto wrong = wrong_parser.parse_begin_statement();
    expect(!wrong && wrong.error().code == ParseErrorCode::expected_begin,
           "dedicated BEGIN parser requires its keyword");
}

void test_insert() {
    Parser parser{"INSERT INTO users VALUES (1, 'Alice', NULL);"};
    const auto result = parser.parse_insert_statement();

    expect(result.has_value(), "valid INSERT is parsed");
    if (!result) {
        return;
    }

    expect(result->table_name == "users", "INSERT table name is preserved");
    expect(result->table_location == SourceLocation{12, 1, 13},
           "INSERT table location is preserved");
    expect(result->values.size() == 3, "INSERT contains three values");
    if (result->values.size() == 3) {
        expect(result->values[0].type == LiteralType::integer,
               "INSERT integer value is parsed");
        expect(result->values[1].type == LiteralType::string,
               "INSERT text value is parsed");
        expect(result->values[2].type == LiteralType::null,
               "INSERT NULL value is parsed");
    }
}

void expect_insert_error(std::string_view source, ParseErrorCode code,
                         SourceLocation location, std::string_view description) {
    Parser parser{source};
    const auto result = parser.parse_insert_statement();

    expect(!result, description);
    if (!result) {
        expect(result.error().code == code, "INSERT error has expected code");
        expect(result.error().location == location, "INSERT error has expected location");
    }
}

void test_insert_errors() {
    expect_insert_error("INTO users VALUES (1);", ParseErrorCode::expected_insert,
                        SourceLocation{0, 1, 1}, "INSERT keyword is required");
    expect_insert_error("INSERT users VALUES (1);", ParseErrorCode::expected_into,
                        SourceLocation{7, 1, 8}, "INTO keyword is required");
    expect_insert_error("INSERT INTO VALUES (1);", ParseErrorCode::expected_identifier,
                        SourceLocation{12, 1, 13}, "INSERT table name is required");
    expect_insert_error("INSERT INTO users (1);", ParseErrorCode::expected_values,
                        SourceLocation{18, 1, 19}, "VALUES keyword is required");
    expect_insert_error("INSERT INTO users VALUES ();", ParseErrorCode::expected_literal,
                        SourceLocation{26, 1, 27}, "INSERT requires at least one value");
}

void test_delete() {
    Parser parser{
        "DELETE FROM users WHERE id = 1 OR name IS NULL;"};
    const auto result = parser.parse_delete_statement();
    expect(result && result->from.name == "users" &&
               result->from.location == SourceLocation{12, 1, 13},
           "DELETE preserves its table reference");
    expect(result && result->where &&
               std::holds_alternative<LogicalExpression>(
                   result->where->node),
           "DELETE reuses logical WHERE predicates");

    Parser all_parser{"DELETE FROM users;"};
    const auto all = all_parser.parse_delete_statement();
    expect(all && !all->where, "DELETE allows an omitted WHERE clause");
}

void test_delete_errors() {
    Parser missing_delete{"FROM users;"};
    const auto no_delete = missing_delete.parse_delete_statement();
    expect(!no_delete &&
               no_delete.error().code == ParseErrorCode::expected_delete,
           "DELETE statement requires DELETE keyword");

    Parser missing_from{"DELETE users;"};
    const auto no_from = missing_from.parse_delete_statement();
    expect(!no_from && no_from.error().code == ParseErrorCode::expected_from,
           "DELETE statement requires FROM keyword");

    Parser missing_table{"DELETE FROM;"};
    const auto no_table = missing_table.parse_delete_statement();
    expect(!no_table &&
               no_table.error().code == ParseErrorCode::expected_identifier,
           "DELETE statement requires a table name");
}

void test_update() {
    Parser parser{
        "UPDATE users SET name = 'Alice', age = 30 WHERE id = 1;"};
    const auto result = parser.parse_update_statement();
    expect(result && result->table.name == "users" &&
               result->table.location == SourceLocation{7, 1, 8},
           "UPDATE preserves its table reference");
    expect(result && result->assignments.size() == 2,
           "UPDATE parses multiple assignments");
    if (result && result->assignments.size() == 2) {
        expect(result->assignments[0].column.name == "name" &&
                   result->assignments[0].value.type == LiteralType::string &&
                   result->assignments[1].column.name == "age" &&
                   result->assignments[1].value.type == LiteralType::integer,
               "UPDATE preserves assignment columns and literals");
    }
    expect(result && result->where.has_value(),
           "UPDATE reuses WHERE predicates");

    Parser all_parser{"UPDATE users SET active = NULL;"};
    const auto all = all_parser.parse_update_statement();
    expect(all && !all->where && all->assignments.size() == 1,
           "UPDATE allows an omitted WHERE clause");
}

void test_update_errors() {
    Parser missing_update{"users SET name = 'Alice';"};
    const auto no_update = missing_update.parse_update_statement();
    expect(!no_update &&
               no_update.error().code == ParseErrorCode::expected_update,
           "UPDATE statement requires UPDATE keyword");

    Parser missing_table{"UPDATE SET name = 'Alice';"};
    const auto no_table = missing_table.parse_update_statement();
    expect(!no_table &&
               no_table.error().code == ParseErrorCode::expected_identifier,
           "UPDATE statement requires a table name");

    Parser missing_set{"UPDATE users name = 'Alice';"};
    const auto no_set = missing_set.parse_update_statement();
    expect(!no_set && no_set.error().code == ParseErrorCode::expected_set,
           "UPDATE statement requires SET keyword");

    Parser missing_equal{"UPDATE users SET name 'Alice';"};
    const auto no_equal = missing_equal.parse_update_statement();
    expect(!no_equal &&
               no_equal.error().code == ParseErrorCode::expected_equal,
           "UPDATE assignment requires equals sign");

    Parser missing_value{"UPDATE users SET name = ;"};
    const auto no_value = missing_value.parse_update_statement();
    expect(!no_value &&
               no_value.error().code == ParseErrorCode::expected_literal,
           "UPDATE assignment requires a literal value");
}

void test_order_by() {
    Parser parser{
        "SELECT name FROM users WHERE id > 0 "
        "ORDER BY age DESC, name ASC;"};
    const auto result = parser.parse_select_statement();
    expect(result && result->where && result->order_by.size() == 2,
           "SELECT parses ORDER BY after WHERE");
    if (result && result->order_by.size() == 2) {
        expect(result->order_by[0].column.name == "age" &&
                   result->order_by[0].direction ==
                       nessodb::sql::OrderDirection::descending &&
                   result->order_by[1].column.name == "name" &&
                   result->order_by[1].direction ==
                       nessodb::sql::OrderDirection::ascending,
               "ORDER BY preserves columns and directions");
    }

    Parser default_parser{"SELECT * FROM users ORDER BY id;"};
    const auto default_order = default_parser.parse_select_statement();
    expect(default_order && default_order->order_by.size() == 1 &&
               default_order->order_by.front().direction ==
                   nessodb::sql::OrderDirection::ascending,
           "ORDER BY defaults to ascending direction");
}

void test_order_by_errors() {
    Parser missing_by{"SELECT * FROM users ORDER id;"};
    const auto no_by = missing_by.parse_select_statement();
    expect(!no_by && no_by.error().code == ParseErrorCode::expected_by,
           "ORDER requires BY keyword");

    Parser missing_column{"SELECT * FROM users ORDER BY;"};
    const auto no_column = missing_column.parse_select_statement();
    expect(!no_column &&
               no_column.error().code == ParseErrorCode::expected_identifier,
           "ORDER BY requires a column");

    Parser missing_comma{"SELECT * FROM users ORDER BY id name;"};
    const auto no_comma = missing_comma.parse_select_statement();
    expect(!no_comma &&
               no_comma.error().code == ParseErrorCode::expected_comma_or_end,
           "ORDER BY terms require commas");
}

void test_group_by() {
    Parser parser{
        "SELECT category, COUNT(*) FROM events WHERE amount > 0 "
        "GROUP BY category, region ORDER BY category LIMIT 2;"};
    const auto result = parser.parse_select_statement();
    expect(result && result->where && result->group_by.size() == 2 &&
               result->order_by.size() == 1 && result->limit,
           "SELECT parses GROUP BY between WHERE and ORDER BY");
    if (result && result->group_by.size() == 2) {
        expect(std::get<ColumnReferenceExpression>(
                   result->group_by[0].node).name == "category" &&
                   std::get<ColumnReferenceExpression>(
                       result->group_by[1].node).name == "region",
               "GROUP BY preserves multiple expressions");
    }

    Parser expression_parser{
        "SELECT amount + 1 FROM events GROUP BY amount + 1;"};
    const auto expression = expression_parser.parse_select_statement();
    expect(expression && expression->group_by.size() == 1 &&
               std::holds_alternative<BinaryArithmeticExpression>(
                   expression->group_by[0].node),
           "GROUP BY accepts value expressions");

    Parser one_row_parser{"SELECT 1 GROUP BY 1 LIMIT 1;"};
    const auto one_row = one_row_parser.parse_select_statement();
    expect(one_row && one_row->group_by.size() == 1 && one_row->limit,
           "GROUP BY works with the implicit one-row SELECT source");

    Parser missing_by{"SELECT category FROM events GROUP category;"};
    const auto invalid = missing_by.parse_select_statement();
    expect(!invalid && invalid.error().code == ParseErrorCode::expected_by,
           "GROUP requires BY keyword");
}

void test_join() {
    Parser parser{
        "SELECT u.name, o.item FROM users u "
        "JOIN orders AS o ON u.id = o.user_id "
        "JOIN shipments s ON o.id = s.order_id "
        "WHERE s.status = 'sent';"};
    const auto result = parser.parse_select_statement();
    expect(result && result->joins.size() == 2 && result->where,
           "SELECT parses chained JOIN clauses before WHERE");
    if (result && result->joins.size() == 2) {
        expect(result->joins[0].table.name == "orders" &&
                   result->joins[0].table.alias &&
                   result->joins[0].table.alias->name == "o" &&
                   std::holds_alternative<ComparisonExpression>(
                       result->joins[0].condition.node) &&
                   result->joins[1].table.name == "shipments",
               "JOIN preserves tables, aliases, and ON expressions");
    }

    Parser missing_on{
        "SELECT * FROM users u JOIN orders o u.id = o.user_id;"};
    const auto invalid = missing_on.parse_select_statement();
    expect(!invalid && invalid.error().code == ParseErrorCode::expected_on,
           "JOIN requires an ON clause");
}

void test_limit() {
    Parser parser{
        "SELECT name FROM users WHERE id > 0 ORDER BY name LIMIT 2 OFFSET 1;"};
    const auto result = parser.parse_select_statement();
    expect(result && result->limit && result->limit->count == "2",
           "SELECT parses LIMIT after ORDER BY");
    expect(result && result->offset && result->offset->count == "1",
           "SELECT parses OFFSET after LIMIT");

    Parser literal_parser{"SELECT 1 LIMIT 0;"};
    const auto literal = literal_parser.parse_select_statement();
    expect(literal && literal->limit && literal->limit->count == "0",
           "SELECT without FROM accepts LIMIT");

    Parser offset_parser{"SELECT * FROM users OFFSET 3;"};
    const auto offset = offset_parser.parse_select_statement();
    expect(offset && offset->offset && offset->offset->count == "3",
           "SELECT accepts OFFSET without LIMIT");

    Parser missing_count{"SELECT * FROM users LIMIT;"};
    const auto missing = missing_count.parse_select_statement();
    expect(!missing &&
               missing.error().code ==
                   ParseErrorCode::expected_integer_literal,
           "LIMIT requires an integer count");
}

void test_distinct() {
    Parser parser{"SELECT DISTINCT name FROM users;"};
    const auto result = parser.parse_select_statement();
    expect(result && result->distinct && result->expressions.size() == 1,
           "SELECT DISTINCT marks a projected query");

    Parser wildcard_parser{"SELECT DISTINCT * FROM users;"};
    const auto wildcard = wildcard_parser.parse_select_statement();
    expect(wildcard && wildcard->distinct && wildcard->select_all_columns,
           "SELECT DISTINCT supports wildcard projection");

    Parser regular_parser{"SELECT name FROM users;"};
    const auto regular = regular_parser.parse_select_statement();
    expect(regular && !regular->distinct,
           "SELECT without DISTINCT keeps all rows");
}

void test_explain() {
    Parser parser{"EXPLAIN SELECT name FROM users LIMIT 2;"};
    const auto result = parser.parse_explain_statement();
    expect(result && result->select.from &&
               result->select.from->name == "users" &&
               result->select.limit &&
               result->select.limit->count == "2",
           "EXPLAIN preserves its SELECT statement");

    Parser invalid{"EXPLAIN DELETE FROM users;"};
    const auto rejected = invalid.parse_explain_statement();
    expect(!rejected &&
               rejected.error().code == ParseErrorCode::expected_select,
           "EXPLAIN accepts only SELECT statements");
}

void test_arithmetic_expressions() {
    Parser parser{"SELECT 1 + 2 * 3, -(4 - 5);"};
    const auto result = parser.parse_select_statement();
    expect(result && result->expressions.size() == 2,
           "arithmetic projections are parsed");
    if (!result || result->expressions.size() != 2) {
        return;
    }

    const auto* addition = std::get_if<BinaryArithmeticExpression>(
        &result->expressions[0].node);
    expect(addition &&
               addition->operation == BinaryArithmeticOperator::addition &&
               std::holds_alternative<BinaryArithmeticExpression>(
                   addition->right->node),
           "multiplication binds more tightly than addition");
    if (addition) {
        const auto* multiplication =
            std::get_if<BinaryArithmeticExpression>(
                &addition->right->node);
        expect(multiplication &&
                   multiplication->operation ==
                       BinaryArithmeticOperator::multiplication,
               "multiplicative expression retains its operation");
    }

    const auto* negation = std::get_if<UnaryArithmeticExpression>(
        &result->expressions[1].node);
    expect(negation &&
               negation->operation == UnaryArithmeticOperator::minus &&
               std::holds_alternative<BinaryArithmeticExpression>(
                   negation->operand->node),
           "parenthesized expression is the unary operand");

    Parser missing_parenthesis{"SELECT (1 + 2;"};
    const auto invalid = missing_parenthesis.parse_select_statement();
    expect(!invalid && invalid.error().code ==
                           ParseErrorCode::expected_right_parenthesis,
           "arithmetic grouping requires a closing parenthesis");
}

void test_expression_aliases() {
    Parser parser{
        "SELECT name AS display_name, 1 + 2 AS total FROM users;"};
    const auto result = parser.parse_select_statement();
    expect(result && result->expressions.size() == 2,
           "explicit expression aliases are parsed");
    if (result && result->expressions.size() == 2) {
        expect(result->expressions[0].alias &&
                   result->expressions[0].alias->name == "display_name" &&
                   result->expressions[0].alias->location ==
                       SourceLocation{15, 1, 16},
               "column alias preserves its name and location");
        expect(result->expressions[1].alias &&
                   result->expressions[1].alias->name == "total",
               "arithmetic expression can have an alias");
    }

    Parser missing_alias{"SELECT 1 AS;"};
    const auto invalid = missing_alias.parse_select_statement();
    expect(!invalid && invalid.error().code ==
                           ParseErrorCode::expected_identifier,
           "AS requires an alias name");
}

void test_predicate_expressions() {
    Parser parser{
        "SELECT id FROM items WHERE price * quantity >= 100 "
        "AND discount + 1 < price;"};
    const auto result = parser.parse_select_statement();
    const auto* logical =
        result && result->where
            ? std::get_if<LogicalExpression>(&result->where->node)
            : nullptr;
    expect(logical && logical->left && logical->right,
           "WHERE accepts recursive scalar expressions");
    if (logical && logical->left && logical->right) {
        const auto& comparison =
            std::get<ComparisonExpression>(logical->left->node);
        expect(comparison.left &&
                   std::holds_alternative<BinaryArithmeticExpression>(
                       comparison.left->node),
               "comparison operands preserve arithmetic trees");
    }

    Parser reversed{"SELECT id FROM items WHERE 1 = id;"};
    expect(reversed.parse_select_statement().has_value(),
           "a literal can appear on the left of a comparison");
}

void test_qualified_columns() {
    Parser parser{
        "SELECT Users.name, users.id + 1 FROM users "
        "WHERE USERS.id = 7 ORDER BY users.name;"};
    const auto result = parser.parse_select_statement();
    expect(result && result->expressions.size() == 2 && result->where &&
               result->order_by.size() == 1,
           "qualified columns are parsed in every SELECT clause");
    if (result && result->expressions.size() == 2 && result->where &&
        result->order_by.size() == 1) {
        const auto& projected =
            std::get<ColumnReferenceExpression>(result->expressions[0].node);
        expect(projected.name == "name" && projected.qualifier &&
                   projected.qualifier->name == "Users" &&
                   projected.qualifier->location == SourceLocation{7, 1, 8},
               "projection preserves qualifier spelling and location");

        const auto& addition =
            std::get<BinaryArithmeticExpression>(result->expressions[1].node);
        const auto& operand =
            std::get<ColumnReferenceExpression>(addition.left->node);
        expect(operand.name == "id" && operand.qualifier &&
                   operand.qualifier->name == "users",
               "arithmetic operands can be qualified");

        const auto& predicate =
            std::get<ComparisonExpression>(result->where->node);
        const auto& predicate_column =
            std::get<ColumnReferenceExpression>(predicate.left->node);
        expect(predicate_column.qualifier &&
                   predicate_column.qualifier->name == "USERS",
               "WHERE columns can be qualified");
        expect(result->order_by[0].column.qualifier &&
                   result->order_by[0].column.qualifier->name == "users",
               "ORDER BY columns can be qualified");
    }

    Parser missing_name{"SELECT users. FROM users;"};
    const auto invalid = missing_name.parse_select_statement();
    expect(!invalid && invalid.error().code ==
                           ParseErrorCode::expected_identifier,
           "a qualifier requires a column name");

    Parser explicit_alias{
        "SELECT u.name FROM users AS u WHERE u.id = 1;"};
    const auto explicit_result = explicit_alias.parse_select_statement();
    expect(explicit_result && explicit_result->from &&
               explicit_result->from->alias &&
               explicit_result->from->alias->name == "u",
           "FROM parses an explicit table alias");

    Parser implicit_alias{"SELECT * FROM users u;"};
    const auto implicit_result = implicit_alias.parse_select_statement();
    expect(implicit_result && implicit_result->from &&
               implicit_result->from->alias &&
               implicit_result->from->alias->name == "u",
           "FROM parses an implicit table alias");

    Parser missing_alias{"SELECT * FROM users AS;"};
    const auto missing_alias_result =
        missing_alias.parse_select_statement();
    expect(!missing_alias_result &&
               missing_alias_result.error().code ==
                   ParseErrorCode::expected_identifier,
           "table AS requires an alias name");
}

}  // namespace

int main() {
    test_select_list();
    test_optional_semicolon();
    test_select_all_from_table();
    test_select_columns();
    test_select_where();
    test_select_null_predicates();
    test_select_and_predicate();
    test_select_or_precedence();
    test_grouped_predicate();
    test_negated_predicate();
    test_errors();
    test_create_table();
    test_create_table_errors();
    test_statement_dispatch();
    test_transaction_control();
    test_insert();
    test_insert_errors();
    test_delete();
    test_delete_errors();
    test_update();
    test_update_errors();
    test_order_by();
    test_order_by_errors();
    test_group_by();
    test_join();
    test_limit();
    test_distinct();
    test_explain();
    test_arithmetic_expressions();
    test_expression_aliases();
    test_predicate_expressions();
    test_qualified_columns();

    if (failures != 0) {
        std::cerr << failures << " parser assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
