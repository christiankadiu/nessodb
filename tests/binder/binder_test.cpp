#include "binder/binder.hpp"
#include "catalog/catalog.hpp"
#include "sql/parser.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using nessodb::binder::BindErrorCode;
using nessodb::binder::BoundColumnReferenceExpression;
using nessodb::binder::BoundComparisonOperator;
using nessodb::binder::BoundComparisonExpression;
using nessodb::binder::BoundLiteralExpression;
using nessodb::binder::BoundLogicalOperator;
using nessodb::binder::BoundLogicalExpression;
using nessodb::binder::BoundNegationExpression;
using nessodb::binder::BoundNullTestExpression;
using nessodb::binder::BoundExpression;
using nessodb::binder::bind_delete_statement;
using nessodb::binder::bind_insert_statement;
using nessodb::binder::bind_select_statement;
using nessodb::binder::bind_update_statement;
using nessodb::catalog::Catalog;
using nessodb::catalog::ColumnSchema;
using nessodb::catalog::TableSchema;
using nessodb::sql::Parser;
using nessodb::sql::SourceLocation;
using nessodb::types::LogicalType;
using nessodb::types::NullValue;

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

const BoundLiteralExpression& literal(const BoundExpression& expression) {
    return std::get<BoundLiteralExpression>(expression.node);
}

const BoundColumnReferenceExpression& column(
    const BoundExpression& expression) {
    return std::get<BoundColumnReferenceExpression>(expression.node);
}

void test_integer_literals() {
    Catalog catalog;
    Parser parser{"SELECT 0, 9223372036854775807;"};
    const auto parsed = parser.parse_select_statement();
    expect(parsed.has_value(), "test query is parsed");
    if (!parsed) {
        return;
    }

    const auto bound = bind_select_statement(*parsed, catalog);
    expect(bound.has_value(), "integer literals are bound");
    if (!bound) {
        return;
    }

    expect(bound->expressions.size() == 2, "two expressions are bound");
    if (bound->expressions.size() == 2) {
        expect(std::get<std::int64_t>(literal(bound->expressions[0]).value) == 0,
               "zero is converted");
        expect(std::get<std::int64_t>(literal(bound->expressions[1]).value) ==
                   std::numeric_limits<std::int64_t>::max(),
               "maximum signed integer is converted");
        expect(literal(bound->expressions[1]).location == SourceLocation{10, 1, 11},
               "source location is preserved");
    }
}

void test_integer_overflow() {
    Catalog catalog;
    Parser parser{"SELECT 9223372036854775808;"};
    const auto parsed = parser.parse_select_statement();
    expect(parsed.has_value(), "overflowing literal is syntactically valid");
    if (!parsed) {
        return;
    }

    const auto bound = bind_select_statement(*parsed, catalog);
    expect(!bound, "overflowing literal is rejected during binding");
    if (!bound) {
        expect(bound.error().code == BindErrorCode::integer_out_of_range,
               "overflow has expected error code");
        expect(bound.error().location == SourceLocation{7, 1, 8},
               "overflow has expected location");
    }
}

void test_string_literals() {
    Catalog catalog;
    Parser parser{"SELECT 'Alice', 'it''s done', '';"};
    const auto parsed = parser.parse_select_statement();
    expect(parsed.has_value(), "string literal is syntactically valid");
    if (!parsed) {
        return;
    }

    const auto bound = bind_select_statement(*parsed, catalog);
    expect(bound.has_value(), "string literals are bound");
    if (!bound) {
        return;
    }

    expect(bound->expressions.size() == 3, "three strings are bound");
    if (bound->expressions.size() == 3) {
        expect(std::get<std::string>(literal(bound->expressions[0]).value) == "Alice",
               "quotes are removed");
        expect(std::get<std::string>(literal(bound->expressions[1]).value) == "it's done",
               "escaped quote is decoded");
        expect(std::get<std::string>(literal(bound->expressions[2]).value).empty(),
               "empty string is decoded");
    }
}

void test_null_literal() {
    Catalog catalog;
    Parser parser{"SELECT NULL;"};
    const auto parsed = parser.parse_select_statement();
    expect(parsed.has_value(), "NULL literal is syntactically valid");
    if (!parsed) {
        return;
    }

    const auto bound = bind_select_statement(*parsed, catalog);
    expect(bound.has_value(), "NULL literal is bound");
    if (!bound) {
        return;
    }

    expect(bound->expressions.size() == 1, "one NULL expression is bound");
    if (bound->expressions.size() == 1) {
        expect(std::holds_alternative<NullValue>(literal(bound->expressions[0]).value),
               "bound value contains NULL");
        expect(literal(bound->expressions[0]).location == SourceLocation{7, 1, 8},
               "NULL source location is preserved");
    }
}

void test_create_table_binding() {
    Parser parser{"CREATE TABLE users(id INT, name TEXT);"};
    const auto parsed = parser.parse_create_table_statement();
    expect(parsed.has_value(), "CREATE TABLE test query is parsed");
    if (!parsed) {
        return;
    }

    const auto bound = nessodb::binder::bind_create_table_statement(*parsed);
    expect(bound.has_value(), "CREATE TABLE is bound");
    if (!bound) {
        return;
    }

    expect(bound->table_name == "users", "bound table name is owned");
    expect(bound->columns.size() == 2, "two columns are bound");
    if (bound->columns.size() == 2) {
        expect(bound->columns[0].name == "id", "first bound column name is preserved");
        expect(bound->columns[0].type == LogicalType::integer,
               "INT maps to logical integer");
        expect(bound->columns[1].name == "name", "second bound column name is preserved");
        expect(bound->columns[1].type == LogicalType::text, "TEXT maps to logical text");
    }
}

void test_duplicate_columns() {
    Parser parser{"CREATE TABLE users(id INT, ID TEXT);"};
    const auto parsed = parser.parse_create_table_statement();
    expect(parsed.has_value(), "duplicate column query is syntactically valid");
    if (!parsed) {
        return;
    }

    const auto bound = nessodb::binder::bind_create_table_statement(*parsed);
    expect(!bound, "duplicate column names are rejected");
    if (!bound) {
        expect(bound.error().code == BindErrorCode::duplicate_column,
               "duplicate column has expected error code");
        expect(bound.error().location == SourceLocation{27, 1, 28},
               "duplicate column has expected location");
    }
}

Catalog make_catalog() {
    Catalog catalog;
    const auto created = catalog.create_table(TableSchema{
        "users",
        {ColumnSchema{"id", LogicalType::integer}, ColumnSchema{"name", LogicalType::text}},
    });
    expect(created.has_value(), "INSERT test table is created");
    return catalog;
}

Catalog make_join_catalog() {
    Catalog catalog = make_catalog();
    const auto orders = catalog.create_table(TableSchema{
        "orders",
        {ColumnSchema{"id", LogicalType::integer},
         ColumnSchema{"user_id", LogicalType::integer},
         ColumnSchema{"item", LogicalType::text}},
    });
    const auto shipments = catalog.create_table(TableSchema{
        "shipments",
        {ColumnSchema{"order_id", LogicalType::integer},
         ColumnSchema{"status", LogicalType::text}},
    });
    expect(orders.has_value() && shipments.has_value(),
           "JOIN test tables are created");
    return catalog;
}

void test_select_from_binding() {
    Catalog catalog = make_catalog();

    Parser parser{"SELECT * FROM users;"};
    const auto parsed = parser.parse_select_statement();
    expect(parsed.has_value(), "table SELECT test query is parsed");
    if (parsed) {
        const auto bound = bind_select_statement(*parsed, catalog);
        expect(bound.has_value(), "table SELECT is bound");
        const auto* table = catalog.find_table("users");
        if (bound && table != nullptr) {
            expect(bound->table_id == table->id, "table SELECT resolves its table identifier");
            expect(bound->expressions.empty(), "wildcard binding has no literal expressions");
            expect(bound->result_column_names ==
                       std::vector<std::string>{"id", "name"},
                   "wildcard binding exposes schema column names");
        }
    }

    Parser projection_parser{
        "SELECT name AS display_name, ID, 'active' AS status FROM USERS;"};
    const auto projection_statement =
        projection_parser.parse_select_statement();
    expect(projection_statement.has_value(), "column projection is parsed");
    if (projection_statement) {
        const auto projection =
            bind_select_statement(*projection_statement, catalog);
        expect(projection && projection->expressions.size() == 3,
               "column projection is bound");
        if (projection && projection->expressions.size() == 3) {
            expect(std::get<BoundColumnReferenceExpression>(
                       projection->expressions[0].node).column_index == 1 &&
                       std::get<BoundColumnReferenceExpression>(
                           projection->expressions[1].node).column_index == 0,
                   "columns resolve case-insensitively to schema positions");
            expect(projection->expressions[0].value_type() ==
                           LogicalType::text &&
                       projection->expressions[1].value_type() ==
                           LogicalType::integer,
                   "bound columns retain their schema value types");
            expect(std::get<std::string>(
                       literal(projection->expressions[2]).value) == "active",
                   "literal projection remains bound");
            expect(projection->result_column_names ==
                       std::vector<std::string>{
                           "display_name", "id", "status"},
                   "binding resolves aliases and default column names");
        }
    }

    Parser predicate_parser{"SELECT name FROM users WHERE ID = 1;"};
    const auto predicate_statement = predicate_parser.parse_select_statement();
    if (predicate_statement) {
        const auto predicate = bind_select_statement(*predicate_statement, catalog);
        const auto* comparison =
            predicate && predicate->where
                ? std::get_if<BoundComparisonExpression>(
                      &predicate->where->node)
                : nullptr;
        expect(comparison && comparison->left && comparison->right &&
                   column(*comparison->left).column_index == 0 &&
                   comparison->comparison ==
                       BoundComparisonOperator::equal &&
                   std::get<std::int64_t>(
                       literal(*comparison->right).value) == 1,
               "WHERE predicate resolves column and literal");
    }

    Parser null_predicate_parser{
        "SELECT id FROM users WHERE name IS NOT NULL;"};
    const auto null_predicate_statement =
        null_predicate_parser.parse_select_statement();
    if (null_predicate_statement) {
        const auto predicate =
            bind_select_statement(*null_predicate_statement, catalog);
        const auto* null_predicate =
            predicate && predicate->where
                ? std::get_if<BoundNullTestExpression>(
                      &predicate->where->node)
                : nullptr;
        expect(null_predicate && null_predicate->operand &&
                   column(*null_predicate->operand).column_index == 1 &&
                   null_predicate->negated,
               "IS NOT NULL resolves column and polarity");
    }

    Parser logical_predicate_parser{
        "SELECT id FROM users WHERE id >= 1 AND name IS NOT NULL;"};
    const auto logical_predicate_statement =
        logical_predicate_parser.parse_select_statement();
    if (logical_predicate_statement) {
        const auto predicate =
            bind_select_statement(*logical_predicate_statement, catalog);
        const auto* logical =
            predicate && predicate->where
                ? std::get_if<BoundLogicalExpression>(
                      &predicate->where->node)
                : nullptr;
        expect(logical && logical->left && logical->right &&
                   logical->operation ==
                       BoundLogicalOperator::conjunction &&
                   column(*std::get<BoundComparisonExpression>(
                               logical->left->node).left).column_index == 0 &&
                   column(*std::get<BoundNullTestExpression>(
                               logical->right->node).operand).column_index == 1,
               "AND binds both predicates to their schema columns");
    }

    Parser disjunction_parser{
        "SELECT id FROM users WHERE id = 1 OR name IS NULL;"};
    const auto disjunction_statement =
        disjunction_parser.parse_select_statement();
    if (disjunction_statement) {
        const auto predicate =
            bind_select_statement(*disjunction_statement, catalog);
        const auto* logical =
            predicate && predicate->where
                ? std::get_if<BoundLogicalExpression>(
                      &predicate->where->node)
                : nullptr;
        expect(logical && logical->operation ==
                       BoundLogicalOperator::disjunction,
               "OR binds as a disjunction");
    }

    Parser negation_parser{
        "SELECT id FROM users WHERE NOT name IS NULL;"};
    const auto negation_statement = negation_parser.parse_select_statement();
    if (negation_statement) {
        const auto predicate =
            bind_select_statement(*negation_statement, catalog);
        const auto* negation =
            predicate && predicate->where
                ? std::get_if<BoundNegationExpression>(
                      &predicate->where->node)
                : nullptr;
        expect(negation && negation->operand &&
                   column(*std::get<BoundNullTestExpression>(
                               negation->operand->node).operand).column_index == 1,
               "NOT binds its operand recursively");
    }

    Parser missing_predicate_parser{
        "SELECT name FROM users WHERE missing = 1;"};
    const auto missing_predicate_statement =
        missing_predicate_parser.parse_select_statement();
    if (missing_predicate_statement) {
        const auto missing_predicate =
            bind_select_statement(*missing_predicate_statement, catalog);
        expect(!missing_predicate && missing_predicate.error().code ==
                                         BindErrorCode::column_not_found,
               "unknown WHERE column is rejected");
    }

    Parser predicate_type_parser{
        "SELECT name FROM users WHERE id = 'one';"};
    const auto predicate_type_statement =
        predicate_type_parser.parse_select_statement();
    if (predicate_type_statement) {
        const auto predicate_type =
            bind_select_statement(*predicate_type_statement, catalog);
        expect(!predicate_type && predicate_type.error().code ==
                                      BindErrorCode::type_mismatch,
               "WHERE literal must match column type");
    }

    Parser unknown_column_parser{"SELECT missing FROM users;"};
    const auto unknown_column_statement =
        unknown_column_parser.parse_select_statement();
    if (unknown_column_statement) {
        const auto unknown_column =
            bind_select_statement(*unknown_column_statement, catalog);
        expect(!unknown_column &&
                   unknown_column.error().code == BindErrorCode::column_not_found,
               "unknown projected column is rejected");
    }

    Parser unqualified_parser{"SELECT id;"};
    const auto unqualified_statement = unqualified_parser.parse_select_statement();
    if (unqualified_statement) {
        const auto unqualified =
            bind_select_statement(*unqualified_statement, catalog);
        expect(!unqualified && unqualified.error().code ==
                                   BindErrorCode::column_requires_table,
               "column reference without FROM is rejected");
    }

    Parser missing_parser{"SELECT * FROM missing;"};
    const auto missing_statement = missing_parser.parse_select_statement();
    expect(missing_statement.has_value(), "unknown-table SELECT is parsed");
    if (missing_statement) {
        const auto missing = bind_select_statement(*missing_statement, catalog);
        expect(!missing && missing.error().code == BindErrorCode::table_not_found,
               "unknown SELECT table is rejected");
    }
}

void test_qualified_column_binding() {
    Catalog catalog = make_catalog();

    Parser parser{
        "SELECT USERS.name, users.id + 1 FROM users "
        "WHERE Users.id = 1 ORDER BY USERS.name;"};
    const auto parsed = parser.parse_select_statement();
    expect(parsed.has_value(), "qualified binding query is parsed");
    if (!parsed) {
        return;
    }
    const auto bound = bind_select_statement(*parsed, catalog);
    expect(bound && bound->expressions.size() == 2 && bound->where &&
               bound->order_by.size() == 1,
           "qualified columns bind across SELECT clauses");
    if (bound && bound->expressions.size() == 2 && bound->where &&
        bound->order_by.size() == 1) {
        expect(column(bound->expressions[0]).column_index == 1 &&
                   bound->order_by[0].column_index == 1,
               "qualified columns resolve to schema positions");
        expect(bound->result_column_names[0] == "name",
               "qualified projection uses the canonical column name");
    }

    Parser wrong_table_parser{"SELECT other.id FROM users;"};
    const auto wrong_table_statement =
        wrong_table_parser.parse_select_statement();
    if (wrong_table_statement) {
        const auto wrong_table =
            bind_select_statement(*wrong_table_statement, catalog);
        expect(!wrong_table && wrong_table.error().code ==
                                   BindErrorCode::table_not_found &&
                   wrong_table.error().location == SourceLocation{7, 1, 8},
               "unknown qualifier is rejected at its location");
    }

    Parser wrong_column_parser{"SELECT users.missing FROM users;"};
    const auto wrong_column_statement =
        wrong_column_parser.parse_select_statement();
    if (wrong_column_statement) {
        const auto wrong_column =
            bind_select_statement(*wrong_column_statement, catalog);
        expect(!wrong_column && wrong_column.error().code ==
                                    BindErrorCode::column_not_found &&
                   wrong_column.error().location ==
                       SourceLocation{13, 1, 14},
               "unknown qualified column is rejected at its location");
    }

    Parser alias_parser{
        "SELECT u.name, COUNT(*) FROM users AS u WHERE u.id >= 1 "
        "GROUP BY u.name ORDER BY u.name;"};
    const auto alias_statement = alias_parser.parse_select_statement();
    if (alias_statement) {
        const auto alias = bind_select_statement(*alias_statement, catalog);
        expect(alias && alias->group_by.size() == 1 &&
                   alias->aggregates.size() == 1 && alias->where &&
                   alias->order_by.size() == 1,
               "table aliases bind across every SELECT clause");
    }

    Parser hidden_name_parser{
        "SELECT users.id FROM users AS u;"};
    const auto hidden_name_statement =
        hidden_name_parser.parse_select_statement();
    if (hidden_name_statement) {
        const auto hidden_name =
            bind_select_statement(*hidden_name_statement, catalog);
        expect(!hidden_name && hidden_name.error().code ==
                                   BindErrorCode::table_not_found,
               "a table alias hides the original table qualifier");
    }
}

void test_insert_binding() {
    Catalog catalog = make_catalog();
    Parser parser{"INSERT INTO users VALUES (1, 'Alice');"};
    const auto parsed = parser.parse_insert_statement();
    expect(parsed.has_value(), "INSERT test query is parsed");
    if (!parsed) {
        return;
    }

    const auto bound = bind_insert_statement(*parsed, catalog);
    expect(bound.has_value(), "INSERT is bound");
    if (!bound) {
        return;
    }

    const auto* table = catalog.find_table("users");
    expect(table != nullptr && bound->table_id == table->id,
           "INSERT resolves the stable table identifier");
    expect(bound->values.size() == 2, "INSERT binds every value");
    if (bound->values.size() == 2) {
        expect(std::get<std::int64_t>(bound->values[0]) == 1,
               "INSERT binds the integer value");
        expect(std::get<std::string>(bound->values[1]) == "Alice",
               "INSERT binds the text value");
    }
}

void test_insert_binding_errors() {
    Catalog catalog = make_catalog();

    Parser missing_parser{"INSERT INTO missing VALUES (1, 'Alice');"};
    const auto missing_statement = missing_parser.parse_insert_statement();
    expect(missing_statement.has_value(), "unknown-table INSERT is parsed");
    if (missing_statement) {
        const auto missing = bind_insert_statement(*missing_statement, catalog);
        expect(!missing && missing.error().code == BindErrorCode::table_not_found,
               "unknown INSERT table is rejected");
    }

    Parser count_parser{"INSERT INTO users VALUES (1);"};
    const auto count_statement = count_parser.parse_insert_statement();
    expect(count_statement.has_value(), "wrong-count INSERT is parsed");
    if (count_statement) {
        const auto count = bind_insert_statement(*count_statement, catalog);
        expect(!count && count.error().code == BindErrorCode::column_count_mismatch,
               "wrong INSERT value count is rejected");
    }

    Parser type_parser{"INSERT INTO users VALUES ('Alice', 'Alice');"};
    const auto type_statement = type_parser.parse_insert_statement();
    expect(type_statement.has_value(), "wrong-type INSERT is parsed");
    if (type_statement) {
        const auto type = bind_insert_statement(*type_statement, catalog);
        expect(!type && type.error().code == BindErrorCode::type_mismatch,
               "wrong INSERT value type is rejected");
        if (!type) {
            expect(type.error().location == type_statement->values[0].location,
                   "type mismatch points to the incompatible value");
        }
    }

    Parser null_parser{"INSERT INTO users VALUES (NULL, NULL);"};
    const auto null_statement = null_parser.parse_insert_statement();
    expect(null_statement.has_value(), "NULL INSERT is parsed");
    if (null_statement) {
        const auto null_bound = bind_insert_statement(*null_statement, catalog);
        expect(null_bound.has_value(), "NULL is compatible with current column types");
    }
}

void test_delete_binding() {
    Catalog catalog = make_catalog();

    Parser parser{
        "DELETE FROM users WHERE id >= 1 AND name IS NOT NULL;"};
    const auto parsed = parser.parse_delete_statement();
    expect(parsed.has_value(), "DELETE is parsed before binding");
    if (!parsed) {
        return;
    }
    const auto bound = bind_delete_statement(*parsed, catalog);
    const auto* table = catalog.find_table("users");
    expect(bound && table && bound->table_id == table->id,
           "DELETE resolves the stable table identifier");
    expect(bound && bound->where &&
               std::holds_alternative<BoundLogicalExpression>(
                   bound->where->node),
           "DELETE binds its WHERE predicate recursively");

    Parser all_parser{"DELETE FROM users;"};
    const auto all_statement = all_parser.parse_delete_statement();
    expect(all_statement.has_value(), "unfiltered DELETE is parsed");
    if (all_statement) {
        const auto all = bind_delete_statement(*all_statement, catalog);
        expect(all && !all->where, "DELETE without WHERE is bound");
    }
}

void test_delete_binding_errors() {
    Catalog catalog = make_catalog();

    Parser missing_parser{"DELETE FROM missing;"};
    const auto missing_statement = missing_parser.parse_delete_statement();
    if (missing_statement) {
        const auto missing = bind_delete_statement(*missing_statement, catalog);
        expect(!missing && missing.error().code == BindErrorCode::table_not_found,
               "DELETE rejects an unknown table");
    }

    Parser column_parser{"DELETE FROM users WHERE missing = 1;"};
    const auto column_statement = column_parser.parse_delete_statement();
    if (column_statement) {
        const auto column = bind_delete_statement(*column_statement, catalog);
        expect(!column && column.error().code == BindErrorCode::column_not_found,
               "DELETE rejects an unknown predicate column");
    }

    Parser type_parser{"DELETE FROM users WHERE id = 'one';"};
    const auto type_statement = type_parser.parse_delete_statement();
    if (type_statement) {
        const auto type = bind_delete_statement(*type_statement, catalog);
        expect(!type && type.error().code == BindErrorCode::type_mismatch,
               "DELETE rejects an incompatible predicate value");
    }
}

void test_update_binding() {
    Catalog catalog = make_catalog();

    Parser parser{
        "UPDATE users SET name = 'Alice', id = 7 WHERE id >= 1;"};
    const auto parsed = parser.parse_update_statement();
    expect(parsed.has_value(), "UPDATE is parsed before binding");
    if (!parsed) {
        return;
    }

    const auto bound = bind_update_statement(*parsed, catalog);
    const auto* table = catalog.find_table("users");
    expect(bound && table && bound->table_id == table->id,
           "UPDATE resolves the stable table identifier");
    expect(bound && bound->assignments.size() == 2,
           "UPDATE binds every assignment");
    if (bound && bound->assignments.size() == 2) {
        expect(bound->assignments[0].column_index == 1 &&
                   std::get<std::string>(bound->assignments[0].value) ==
                       "Alice" &&
                   bound->assignments[1].column_index == 0 &&
                   std::get<std::int64_t>(bound->assignments[1].value) == 7,
               "UPDATE resolves assignment columns and typed values");
    }
    expect(bound && bound->where.has_value(),
           "UPDATE binds its WHERE predicate");

    Parser all_parser{"UPDATE users SET name = NULL;"};
    const auto all_statement = all_parser.parse_update_statement();
    if (all_statement) {
        const auto all = bind_update_statement(*all_statement, catalog);
        expect(all && !all->where &&
                   std::holds_alternative<NullValue>(
                       all->assignments.front().value),
               "unfiltered UPDATE binds NULL assignment");
    }
}

void test_update_binding_errors() {
    Catalog catalog = make_catalog();

    Parser missing_parser{"UPDATE missing SET id = 1;"};
    const auto missing_statement = missing_parser.parse_update_statement();
    if (missing_statement) {
        const auto missing =
            bind_update_statement(*missing_statement, catalog);
        expect(!missing &&
                   missing.error().code == BindErrorCode::table_not_found,
               "UPDATE rejects an unknown table");
    }

    Parser column_parser{"UPDATE users SET missing = 1;"};
    const auto column_statement = column_parser.parse_update_statement();
    if (column_statement) {
        const auto column = bind_update_statement(*column_statement, catalog);
        expect(!column &&
                   column.error().code == BindErrorCode::column_not_found,
               "UPDATE rejects an unknown assignment column");
    }

    Parser duplicate_parser{"UPDATE users SET name = 'a', NAME = 'b';"};
    const auto duplicate_statement = duplicate_parser.parse_update_statement();
    if (duplicate_statement) {
        const auto duplicate =
            bind_update_statement(*duplicate_statement, catalog);
        expect(!duplicate &&
                   duplicate.error().code == BindErrorCode::duplicate_column,
               "UPDATE rejects duplicate assignments case-insensitively");
    }

    Parser type_parser{"UPDATE users SET id = 'one';"};
    const auto type_statement = type_parser.parse_update_statement();
    if (type_statement) {
        const auto type = bind_update_statement(*type_statement, catalog);
        expect(!type && type.error().code == BindErrorCode::type_mismatch,
               "UPDATE rejects an incompatible assignment value");
    }

    Parser predicate_parser{"UPDATE users SET name = 'a' WHERE id = 'one';"};
    const auto predicate_statement = predicate_parser.parse_update_statement();
    if (predicate_statement) {
        const auto predicate =
            bind_update_statement(*predicate_statement, catalog);
        expect(!predicate &&
                   predicate.error().code == BindErrorCode::type_mismatch,
               "UPDATE rejects an incompatible predicate value");
    }
}

void test_order_by_binding() {
    Catalog catalog = make_catalog();

    Parser parser{"SELECT name FROM users ORDER BY id DESC, name;"};
    const auto parsed = parser.parse_select_statement();
    expect(parsed.has_value(), "ORDER BY is parsed before binding");
    if (parsed) {
        const auto bound = bind_select_statement(*parsed, catalog);
        expect(bound && bound->order_by.size() == 2 &&
                   bound->order_by[0].column_index == 0 &&
                   bound->order_by[0].direction ==
                       nessodb::binder::BoundOrderDirection::descending &&
                   bound->order_by[1].column_index == 1,
               "ORDER BY resolves non-projected columns and directions");
    }

    Parser missing_parser{"SELECT name FROM users ORDER BY missing;"};
    const auto missing_statement = missing_parser.parse_select_statement();
    if (missing_statement) {
        const auto missing =
            bind_select_statement(*missing_statement, catalog);
        expect(!missing &&
                   missing.error().code == BindErrorCode::column_not_found,
               "ORDER BY rejects an unknown column");
    }
}

void test_limit_binding() {
    Catalog catalog = make_catalog();
    Parser parser{"SELECT * FROM users LIMIT 2 OFFSET 1;"};
    const auto parsed = parser.parse_select_statement();
    expect(parsed.has_value(), "LIMIT is parsed before binding");
    if (parsed) {
        const auto bound = bind_select_statement(*parsed, catalog);
        expect(bound && bound->limit && *bound->limit == 2 &&
                   bound->offset && *bound->offset == 1,
               "LIMIT and OFFSET counts become execution bounds");
    }
}

void test_distinct_binding() {
    Catalog catalog = make_catalog();
    Parser parser{"SELECT DISTINCT name FROM users;"};
    const auto parsed = parser.parse_select_statement();
    expect(parsed.has_value(), "DISTINCT is parsed before binding");
    if (parsed) {
        const auto bound = bind_select_statement(*parsed, catalog);
        expect(bound && bound->distinct,
               "binder preserves the DISTINCT modifier");
    }
}

void test_arithmetic_expression_binding() {
    nessodb::sql::SelectStatement statement;
    statement.expressions.emplace_back(
        nessodb::sql::BinaryArithmeticExpression{
            nessodb::sql::BinaryArithmeticOperator::addition,
            std::make_unique<nessodb::sql::Expression>(
                nessodb::sql::LiteralExpression{
                    nessodb::sql::LiteralType::integer, "2", {7, 1, 8}}),
            std::make_unique<nessodb::sql::Expression>(
                nessodb::sql::LiteralExpression{
                    nessodb::sql::LiteralType::null, "NULL", {11, 1, 12}}),
            {9, 1, 10}});

    Catalog catalog;
    const auto bound = bind_select_statement(statement, catalog);
    expect(bound && bound->expressions.size() == 1,
           "integer arithmetic expression is bound");
    if (bound && bound->expressions.size() == 1) {
        expect(std::holds_alternative<
                   nessodb::binder::BoundBinaryArithmeticExpression>(
                   bound->expressions[0].node),
               "binary arithmetic keeps its bound tree shape");
        expect(bound->expressions[0].value_type() == LogicalType::integer,
               "arithmetic expression has integer value type");
    }

    nessodb::sql::SelectStatement invalid_statement;
    invalid_statement.expressions.emplace_back(
        nessodb::sql::UnaryArithmeticExpression{
            nessodb::sql::UnaryArithmeticOperator::minus,
            std::make_unique<nessodb::sql::Expression>(
                nessodb::sql::LiteralExpression{
                    nessodb::sql::LiteralType::string, "'text'", {8, 1, 9}}),
            {7, 1, 8}});
    const auto invalid = bind_select_statement(invalid_statement, catalog);
    expect(!invalid && invalid.error().code == BindErrorCode::type_mismatch,
           "arithmetic expression rejects text operands");
}

void test_predicate_expression_binding() {
    Catalog catalog = make_catalog();
    Parser parser{
        "SELECT id FROM users WHERE id * 2 >= 4 AND id = id;"};
    const auto parsed = parser.parse_select_statement();
    expect(parsed.has_value(), "predicate expression query is parsed");
    if (!parsed) {
        return;
    }
    const auto bound = bind_select_statement(*parsed, catalog);
    const auto* logical =
        bound && bound->where
            ? std::get_if<BoundLogicalExpression>(&bound->where->node)
            : nullptr;
    expect(logical && logical->left && logical->right,
           "binder accepts arithmetic and column comparison operands");
    if (logical && logical->left) {
        const auto& comparison =
            std::get<BoundComparisonExpression>(logical->left->node);
        expect(comparison.left &&
                   std::holds_alternative<
                       nessodb::binder::BoundBinaryArithmeticExpression>(
                       comparison.left->node),
               "bound comparison retains its arithmetic operand");
    }

    Parser mismatch_parser{"SELECT id FROM users WHERE id = name;"};
    const auto mismatch_statement = mismatch_parser.parse_select_statement();
    if (mismatch_statement) {
        const auto mismatch =
            bind_select_statement(*mismatch_statement, catalog);
        expect(!mismatch && mismatch.error().code ==
                                BindErrorCode::type_mismatch,
               "comparison operands must have compatible types");
    }

    Parser non_boolean_parser{"SELECT id FROM users WHERE id;"};
    const auto non_boolean_statement =
        non_boolean_parser.parse_select_statement();
    if (non_boolean_statement) {
        const auto non_boolean =
            bind_select_statement(*non_boolean_statement, catalog);
        expect(!non_boolean && non_boolean.error().code ==
                                   BindErrorCode::type_mismatch,
               "WHERE requires a boolean expression");
    }
}

void test_group_by_binding() {
    Catalog catalog = make_catalog();
    Parser parser{
        "SELECT COUNT(*) AS total, name FROM users "
        "GROUP BY name ORDER BY name DESC;"};
    const auto parsed = parser.parse_select_statement();
    expect(parsed.has_value(), "GROUP BY binding query is parsed");
    if (!parsed) {
        return;
    }
    const auto bound = bind_select_statement(*parsed, catalog);
    expect(bound && bound->group_by.size() == 1 &&
               bound->aggregates.size() == 1 &&
               bound->expressions.size() == 2 &&
               bound->order_by.size() == 1,
           "GROUP BY binds keys, aggregates, output, and ordering");
    if (bound && bound->group_by.size() == 1 &&
        bound->expressions.size() == 2 && bound->order_by.size() == 1) {
        expect(column(bound->group_by[0]).column_index == 1 &&
                   column(bound->expressions[0]).column_index == 1 &&
                   column(bound->expressions[1]).column_index == 0 &&
                   bound->order_by[0].column_index == 0,
               "grouped output preserves SELECT order over aggregate rows");
        expect(bound->result_column_names ==
                   std::vector<std::string>{"total", "name"},
               "grouped output preserves aliases and column names");
    }

    Parser expression_parser{
        "SELECT id + 1 FROM users GROUP BY id + 1;"};
    const auto expression_statement =
        expression_parser.parse_select_statement();
    if (expression_statement) {
        const auto expression =
            bind_select_statement(*expression_statement, catalog);
        expect(expression && expression->group_by.size() == 1 &&
                   expression->expressions.size() == 1,
               "matching grouped value expressions are accepted");
    }

    Parser invalid_parser{
        "SELECT id, COUNT(*) FROM users GROUP BY name;"};
    const auto invalid_statement = invalid_parser.parse_select_statement();
    if (invalid_statement) {
        const auto invalid =
            bind_select_statement(*invalid_statement, catalog);
        expect(!invalid && invalid.error().code ==
                               BindErrorCode::column_not_grouped,
               "selected scalar expressions must be group keys");
    }
}

void test_join_binding() {
    Catalog catalog = make_join_catalog();
    Parser parser{
        "SELECT u.name, o.item FROM users u "
        "JOIN orders o ON u.id = o.user_id "
        "WHERE o.id > 0 ORDER BY o.item;"};
    const auto parsed = parser.parse_select_statement();
    expect(parsed.has_value(), "JOIN binding query is parsed");
    if (!parsed) {
        return;
    }
    const auto bound = bind_select_statement(*parsed, catalog);
    expect(bound && bound->joins.size() == 1 &&
               bound->expressions.size() == 2 && bound->where &&
               bound->order_by.size() == 1,
           "JOIN binds sources, ON, projection, WHERE, and ORDER BY");
    if (bound && bound->joins.size() == 1 &&
        bound->expressions.size() == 2) {
        const auto& condition = std::get<BoundComparisonExpression>(
            bound->joins[0].condition.node);
        expect(column(bound->expressions[0]).column_index == 1 &&
                   column(bound->expressions[1]).column_index == 4 &&
                   condition.left && condition.right &&
                   column(*condition.left).column_index == 0 &&
                   column(*condition.right).column_index == 3,
               "joined columns use stable flattened offsets");
    }

    Parser ambiguous_parser{
        "SELECT id FROM users u JOIN orders o ON u.id = o.user_id;"};
    const auto ambiguous_statement =
        ambiguous_parser.parse_select_statement();
    if (ambiguous_statement) {
        const auto ambiguous =
            bind_select_statement(*ambiguous_statement, catalog);
        expect(!ambiguous && ambiguous.error().code ==
                                  BindErrorCode::ambiguous_column,
               "unqualified columns shared by joined tables are ambiguous");
    }

    Parser duplicate_parser{
        "SELECT u.id FROM users u JOIN orders u ON u.id = u.user_id;"};
    const auto duplicate_statement =
        duplicate_parser.parse_select_statement();
    if (duplicate_statement) {
        const auto duplicate =
            bind_select_statement(*duplicate_statement, catalog);
        expect(!duplicate && duplicate.error().code ==
                                  BindErrorCode::duplicate_table,
               "joined tables require distinct visible names");
    }

    Parser missing_parser{
        "SELECT u.id FROM users u JOIN missing m ON u.id = m.id;"};
    const auto missing_statement = missing_parser.parse_select_statement();
    if (missing_statement) {
        const auto missing =
            bind_select_statement(*missing_statement, catalog);
        expect(!missing && missing.error().code ==
                                BindErrorCode::table_not_found,
               "JOIN rejects an unknown table");
    }
}

}  // namespace

int main() {
    test_integer_literals();
    test_integer_overflow();
    test_string_literals();
    test_null_literal();
    test_create_table_binding();
    test_duplicate_columns();
    test_select_from_binding();
    test_qualified_column_binding();
    test_insert_binding();
    test_insert_binding_errors();
    test_delete_binding();
    test_delete_binding_errors();
    test_update_binding();
    test_update_binding_errors();
    test_order_by_binding();
    test_limit_binding();
    test_distinct_binding();
    test_arithmetic_expression_binding();
    test_predicate_expression_binding();
    test_group_by_binding();
    test_join_binding();

    if (failures != 0) {
        std::cerr << failures << " binder assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
