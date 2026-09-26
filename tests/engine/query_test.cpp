#include "engine/query.hpp"
#include "engine/database.hpp"
#include "recovery/wal.hpp"
#include "storage/io/page_file.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using nessodb::binder::BindError;
using nessodb::binder::BindErrorCode;
using nessodb::engine::execute_query;
using nessodb::engine::Database;
using nessodb::engine::ExecutionError;
using nessodb::engine::ExecutionErrorCode;
using nessodb::engine::StorageError;
using nessodb::engine::TransactionExecutionError;
using nessodb::engine::TransactionExecutionErrorCode;
using nessodb::sql::ParseError;
using nessodb::types::NullValue;

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

void remove_database_files(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(nessodb::recovery::wal_path_for(path), ignored);
}

void test_query_result() {
    const auto result = execute_query("SELECT 1, 'Alice', NULL;");

    expect(result.has_value(), "valid query is executed");
    if (!result) {
        return;
    }

    expect(result->rows.size() == 1, "query returns one row");
    expect(result->column_names ==
               std::vector<std::string>{
                   "?column?", "?column?", "?column?"},
           "unnamed literal projections expose stable result names");
    if (result->rows.size() == 1) {
        const auto& values = result->rows[0].values;
        expect(values.size() == 3, "query row contains three values");
        if (values.size() != 3) {
            return;
        }
        expect(std::get<std::int64_t>(values[0]) == std::int64_t{1},
               "integer value is returned");
        expect(std::get<std::string>(values[1]) == "Alice",
               "text value is returned");
        expect(std::holds_alternative<NullValue>(values[2]), "NULL value is returned");
    }
}

void test_query_errors() {
    const auto parse_failure = execute_query("SELECT;");
    expect(!parse_failure, "invalid syntax is rejected");
    if (!parse_failure) {
        expect(std::holds_alternative<ParseError>(parse_failure.error()),
               "syntax failure preserves parse error");
    }

    const auto bind_failure = execute_query("SELECT 9223372036854775808;");
    expect(!bind_failure, "integer overflow is rejected");
    if (!bind_failure) {
        expect(std::holds_alternative<BindError>(bind_failure.error()),
               "overflow preserves bind error");
    }
}

void test_create_table() {
    Database database;

    const auto created = database.execute("CREATE TABLE users(id INT, name TEXT);");
    expect(created.has_value(), "CREATE TABLE is executed");
    if (created) {
        expect(created->rows.empty(), "CREATE TABLE returns no rows");
    }

    const auto duplicate = database.execute("CREATE TABLE USERS(id INT);");
    expect(!duplicate, "duplicate table is rejected");
    if (!duplicate) {
        const auto* error = std::get_if<ExecutionError>(&duplicate.error());
        expect(error != nullptr, "duplicate table preserves execution error");
        if (error != nullptr) {
            expect(error->code == ExecutionErrorCode::table_already_exists,
                   "duplicate table reports its error code");
            expect(error->location == nessodb::sql::SourceLocation{13, 1, 14},
                   "duplicate table reports its name location");
        }
    }

    const auto selected = database.execute("SELECT 1;");
    expect(selected.has_value(), "database remains usable after CREATE TABLE");
}

void test_insert() {
    Database database;
    const auto created = database.execute("CREATE TABLE users(id INT, name TEXT);");
    expect(created.has_value(), "INSERT test table is created");

    const auto inserted = database.execute("INSERT INTO users VALUES (1, 'Alice');");
    expect(inserted.has_value(), "INSERT is executed");
    if (inserted) {
        expect(inserted->rows.empty(), "INSERT returns no rows");
        expect(inserted->rows_affected == 1, "INSERT reports one affected row");
    }

    const auto null_insert = database.execute("INSERT INTO USERS VALUES (NULL, NULL);");
    expect(null_insert.has_value(), "INSERT resolves table names case-insensitively");

    const auto wrong_type = database.execute("INSERT INTO users VALUES ('Alice', 'Alice');");
    expect(!wrong_type, "INSERT rejects incompatible values");
    if (!wrong_type) {
        const auto* error = std::get_if<BindError>(&wrong_type.error());
        expect(error != nullptr && error->code == BindErrorCode::type_mismatch,
               "INSERT preserves type mismatch error");
    }

    const auto missing = database.execute("INSERT INTO missing VALUES (1, 'Alice');");
    expect(!missing, "INSERT rejects an unknown table");
    if (!missing) {
        const auto* error = std::get_if<BindError>(&missing.error());
        expect(error != nullptr && error->code == BindErrorCode::table_not_found,
               "INSERT preserves unknown table error");
    }
}

void test_select_from() {
    Database database;
    const auto created = database.execute("CREATE TABLE users(id INT, name TEXT);");
    expect(created.has_value(), "table SELECT test table is created");

    const auto empty = database.execute("SELECT * FROM users;");
    expect(empty.has_value(), "empty table is scanned");
    if (empty) {
        expect(empty->rows.empty(), "empty table returns no rows");
        expect(empty->column_names ==
                   std::vector<std::string>{"id", "name"},
               "empty wildcard results retain schema column names");
    }

    const auto first = database.execute("INSERT INTO users VALUES (1, 'Alice');");
    const auto second = database.execute("INSERT INTO users VALUES (2, NULL);");
    expect(first.has_value() && second.has_value(), "table SELECT test rows are inserted");

    const auto selected = database.execute("SELECT * FROM USERS;");
    expect(selected.has_value(), "table rows are selected case-insensitively");
    if (!selected) {
        return;
    }
    expect(selected->rows.size() == 2, "table scan returns every row");
    if (selected->rows.size() == 2) {
        expect(std::get<std::int64_t>(selected->rows[0].values[0]) == 1,
               "table scan preserves insertion order");
        expect(std::get<std::string>(selected->rows[0].values[1]) == "Alice",
               "table scan returns text values");
        expect(std::holds_alternative<NullValue>(selected->rows[1].values[1]),
               "table scan returns NULL values");
    }

    const auto projected =
        database.execute(
            "SELECT name AS display_name, ID, 'active' AS status "
            "FROM users;");
    expect(projected && projected->rows.size() == 2,
           "selected columns are projected for every row");
    if (projected && projected->rows.size() == 2) {
        expect(projected->column_names ==
                   std::vector<std::string>{
                       "display_name", "id", "status"},
               "query result exposes aliases and canonical column names");
        expect(projected->rows[0].values.size() == 3 &&
                   std::get<std::string>(projected->rows[0].values[0]) == "Alice" &&
                   std::get<std::int64_t>(projected->rows[0].values[1]) == 1 &&
                   std::get<std::string>(projected->rows[0].values[2]) == "active",
               "projection preserves order and evaluates literals");
        expect(std::holds_alternative<NullValue>(
                   projected->rows[1].values[0]),
               "projection preserves NULL values");
    }

    const auto filtered =
        database.execute("SELECT id FROM users WHERE name = 'Alice';");
    expect(filtered && filtered->rows.size() == 1 &&
               std::get<std::int64_t>(filtered->rows[0].values[0]) == 1,
           "WHERE equality filters rows before projection");

    const auto expect_filtered_id =
        [&database](std::string_view source, std::int64_t expected,
                    std::string_view description) {
            const auto result = database.execute(source);
            expect(result && result->rows.size() == 1 &&
                       std::get<std::int64_t>(result->rows[0].values[0]) ==
                           expected,
                   description);
        };
    expect_filtered_id("SELECT id FROM users WHERE id != 1;", 2,
                       "WHERE supports inequality");
    expect_filtered_id("SELECT id FROM users WHERE id < 2;", 1,
                       "WHERE supports less than");
    expect_filtered_id("SELECT id FROM users WHERE id <= 1;", 1,
                       "WHERE supports less than or equal");
    expect_filtered_id("SELECT id FROM users WHERE id > 1;", 2,
                       "WHERE supports greater than");
    expect_filtered_id("SELECT id FROM users WHERE id >= 2;", 2,
                       "WHERE supports greater than or equal");
    expect_filtered_id("SELECT id FROM users WHERE name >= 'Alice';", 1,
                       "WHERE compares text values lexicographically");

    const auto null_comparison =
        database.execute("SELECT id FROM users WHERE name = NULL;");
    expect(null_comparison && null_comparison->rows.empty(),
           "equality comparison with NULL selects no rows");

    const auto is_null =
        database.execute("SELECT id FROM users WHERE name IS NULL;");
    expect(is_null && is_null->rows.size() == 1 &&
               std::get<std::int64_t>(is_null->rows[0].values[0]) == 2,
           "IS NULL selects rows containing NULL");

    const auto is_not_null =
        database.execute("SELECT id FROM users WHERE name IS NOT NULL;");
    expect(is_not_null && is_not_null->rows.size() == 1 &&
               std::get<std::int64_t>(is_not_null->rows[0].values[0]) == 1,
           "IS NOT NULL excludes rows containing NULL");

    const auto conjunction = database.execute(
        "SELECT id FROM users WHERE id >= 1 AND name IS NOT NULL;");
    expect(conjunction && conjunction->rows.size() == 1 &&
               std::get<std::int64_t>(conjunction->rows[0].values[0]) == 1,
           "AND requires both predicates to match");

    const auto rejected_conjunction = database.execute(
        "SELECT id FROM users WHERE id > 1 AND name IS NOT NULL;");
    expect(rejected_conjunction && rejected_conjunction->rows.empty(),
           "AND rejects rows when either predicate does not match");

    const auto disjunction = database.execute(
        "SELECT id FROM users WHERE id = 1 OR id = 2;");
    expect(disjunction && disjunction->rows.size() == 2,
           "OR accepts rows when either predicate matches");

    const auto precedence = database.execute(
        "SELECT id FROM users WHERE name IS NULL OR id = 1 AND name IS NOT NULL;");
    expect(precedence && precedence->rows.size() == 2,
           "AND is evaluated before OR");

    const auto grouped = database.execute(
        "SELECT id FROM users WHERE (name IS NULL OR id = 1) AND name IS NOT NULL;");
    expect(grouped && grouped->rows.size() == 1 &&
               std::get<std::int64_t>(grouped->rows[0].values[0]) == 1,
           "parentheses override logical precedence");

    const auto negated =
        database.execute("SELECT id FROM users WHERE NOT id = 1;");
    expect(negated && negated->rows.size() == 1 &&
               std::get<std::int64_t>(negated->rows[0].values[0]) == 2,
           "NOT inverts a predicate result");

    const auto negated_group = database.execute(
        "SELECT id FROM users WHERE NOT (id = 1 OR name IS NULL);");
    expect(negated_group && negated_group->rows.empty(),
           "NOT inverts a grouped predicate");

    const auto wrong_predicate_type =
        database.execute("SELECT id FROM users WHERE id = 'one';");
    expect(!wrong_predicate_type,
           "WHERE rejects a literal with incompatible type");
    if (!wrong_predicate_type) {
        const auto* error =
            std::get_if<BindError>(&wrong_predicate_type.error());
        expect(error != nullptr && error->code == BindErrorCode::type_mismatch,
               "WHERE type mismatch preserves bind error");
    }

    const auto unknown_column =
        database.execute("SELECT missing FROM users;");
    expect(!unknown_column, "unknown projected column is rejected");
    if (!unknown_column) {
        const auto* error = std::get_if<BindError>(&unknown_column.error());
        expect(error != nullptr && error->code == BindErrorCode::column_not_found,
               "unknown projected column preserves bind error");
    }

    const auto missing = database.execute("SELECT * FROM missing;");
    expect(!missing, "table scan rejects an unknown table");
    if (!missing) {
        const auto* error = std::get_if<BindError>(&missing.error());
        expect(error != nullptr && error->code == BindErrorCode::table_not_found,
               "unknown table scan preserves bind error");
    }
}

void test_count_queries() {
    Database database;
    expect(database.execute(
               "CREATE TABLE events(id INT, label TEXT);").has_value() &&
               database.execute(
                   "INSERT INTO events VALUES (1, 'one');").has_value() &&
               database.execute(
                   "INSERT INTO events VALUES (2, NULL);").has_value() &&
               database.execute(
                   "INSERT INTO events VALUES (3, 'three');").has_value(),
           "COUNT test data is created");

    const auto counts = database.execute(
        "SELECT COUNT(*) AS rows, COUNT(label) AS labels "
        "FROM events WHERE id >= 2;");
    expect(counts && counts->column_names ==
                         std::vector<std::string>{"rows", "labels"} &&
               counts->rows.size() == 1 &&
               std::get<std::int64_t>(counts->rows[0].values[0]) == 2 &&
               std::get<std::int64_t>(counts->rows[0].values[1]) == 1,
           "COUNT executes with aliases, expressions, NULLs, and filtering");

    const auto aggregates = database.execute(
        "SELECT MIN(id) AS first_id, MAX(label), SUM(id) AS total "
        "FROM events;");
    expect(aggregates && aggregates->column_names ==
                             std::vector<std::string>{
                                 "first_id", "max", "total"} &&
               aggregates->rows.size() == 1 &&
               std::get<std::int64_t>(
                   aggregates->rows[0].values[0]) == 1 &&
               std::get<std::string>(
                   aggregates->rows[0].values[1]) == "three" &&
               std::get<std::int64_t>(
                   aggregates->rows[0].values[2]) == 6,
           "MIN, MAX, and SUM execute with aliases and default names");

    Database empty;
    expect(empty.execute("CREATE TABLE empty_items(id INT);").has_value(),
           "empty COUNT table is created");
    const auto empty_count =
        empty.execute("SELECT COUNT(*) FROM empty_items;");
    expect(empty_count && empty_count->rows.size() == 1 &&
               std::get<std::int64_t>(
                   empty_count->rows[0].values[0]) == 0,
           "COUNT(*) over an empty table returns zero");
    const auto empty_aggregates =
        empty.execute("SELECT MIN(id), MAX(id), SUM(id) FROM empty_items;");
    expect(empty_aggregates && empty_aggregates->rows.size() == 1 &&
               std::holds_alternative<nessodb::types::NullValue>(
                   empty_aggregates->rows[0].values[0]) &&
               std::holds_alternative<nessodb::types::NullValue>(
                   empty_aggregates->rows[0].values[1]) &&
               std::holds_alternative<nessodb::types::NullValue>(
                   empty_aggregates->rows[0].values[2]),
           "MIN, MAX, and SUM return NULL over empty input");

    const auto explained =
        database.execute("EXPLAIN SELECT COUNT(*) FROM events;");
    expect(explained && !explained->rows.empty() &&
               std::get<std::string>(explained->rows[0].values[0]).find(
                   "Global Aggregate") != std::string::npos,
           "EXPLAIN exposes global aggregation");

    const auto mixed =
        database.execute("SELECT COUNT(*), id FROM events;");
    expect(!mixed && std::holds_alternative<BindError>(mixed.error()) &&
               std::get<BindError>(mixed.error()).code ==
                   BindErrorCode::mixed_aggregate_and_scalar,
           "global aggregation rejects scalar columns without GROUP BY");

    const auto unknown = database.execute("SELECT TOTAL(*) FROM events;");
    expect(!unknown && std::holds_alternative<BindError>(unknown.error()) &&
               std::get<BindError>(unknown.error()).code ==
                   BindErrorCode::function_not_found,
           "unknown aggregate functions are rejected during binding");

    const auto invalid_sum =
        database.execute("SELECT SUM(label) FROM events;");
    expect(!invalid_sum &&
               std::holds_alternative<BindError>(invalid_sum.error()) &&
               std::get<BindError>(invalid_sum.error()).code ==
                   BindErrorCode::type_mismatch,
           "SUM rejects text arguments during binding");
}

void test_group_by_queries() {
    Database database;
    expect(database.execute(
               "CREATE TABLE sales(category TEXT, amount INT);").has_value() &&
               database.execute(
                   "INSERT INTO sales VALUES ('north', 10);").has_value() &&
               database.execute(
                   "INSERT INTO sales VALUES (NULL, 2);").has_value() &&
               database.execute(
                   "INSERT INTO sales VALUES ('south', 4);").has_value() &&
               database.execute(
                   "INSERT INTO sales VALUES ('north', 7);").has_value(),
           "GROUP BY test data is created");

    const auto grouped = database.execute(
        "SELECT COUNT(*) AS rows, category, SUM(amount) AS total "
        "FROM sales GROUP BY category ORDER BY category ASC;");
    expect(grouped && grouped->column_names ==
                          std::vector<std::string>{
                              "rows", "category", "total"} &&
               grouped->rows.size() == 3,
           "GROUP BY returns one row per key in SELECT order");
    if (grouped && grouped->rows.size() == 3) {
        expect(std::get<std::int64_t>(grouped->rows[0].values[0]) == 2 &&
                   std::get<std::string>(grouped->rows[0].values[1]) ==
                       "north" &&
                   std::get<std::int64_t>(grouped->rows[0].values[2]) == 17 &&
                   std::get<std::string>(grouped->rows[1].values[1]) ==
                       "south" &&
                   std::holds_alternative<NullValue>(
                       grouped->rows[2].values[1]),
               "group states, ordering, and NULL key semantics are preserved");
    }

    const auto keys_only = database.execute(
        "SELECT category FROM sales GROUP BY category;");
    expect(keys_only && keys_only->rows.size() == 3,
           "GROUP BY works without aggregate functions");

    const auto multiple_keys = database.execute(
        "SELECT category, amount, COUNT(*) FROM sales "
        "GROUP BY category, amount;");
    expect(multiple_keys && multiple_keys->rows.size() == 4 &&
               multiple_keys->rows.front().values.size() == 3,
           "GROUP BY supports multiple keys");

    const auto expression_key = database.execute(
        "SELECT amount + 1, COUNT(*) FROM sales GROUP BY amount + 1;");
    expect(expression_key && expression_key->rows.size() == 4,
           "GROUP BY evaluates matching value expressions");

    const auto explained = database.execute(
        "EXPLAIN SELECT category, COUNT(*) FROM sales GROUP BY category;");
    expect(explained && explained->rows.size() == 3 &&
               std::get<std::string>(explained->rows[1].values[0]).find(
                   "Hash Aggregate (keys=1, aggregates=1)") !=
                   std::string::npos,
           "EXPLAIN exposes grouped hash aggregation");

    const auto invalid = database.execute(
        "SELECT amount, COUNT(*) FROM sales GROUP BY category;");
    expect(!invalid && std::holds_alternative<BindError>(invalid.error()) &&
               std::get<BindError>(invalid.error()).code ==
                   BindErrorCode::column_not_grouped,
           "GROUP BY rejects selected columns outside the grouping keys");

    Database empty;
    expect(empty.execute(
               "CREATE TABLE empty_sales(category TEXT);").has_value(),
           "empty GROUP BY table is created");
    const auto empty_groups = empty.execute(
        "SELECT category, COUNT(*) FROM empty_sales GROUP BY category;");
    expect(empty_groups && empty_groups->rows.empty(),
           "GROUP BY over empty input emits no groups");
}

void test_predicate_expression_queries() {
    Database database;
    expect(database.execute(
               "CREATE TABLE items(price INT, quantity INT);").has_value() &&
               database.execute(
                   "INSERT INTO items VALUES (10, 2);").has_value() &&
               database.execute(
                   "INSERT INTO items VALUES (4, 3);").has_value(),
           "predicate expression test data is created");

    const auto result = database.execute(
        "SELECT price * quantity AS total FROM items "
        "WHERE price * quantity >= 20;");
    expect(result && result->rows.size() == 1 &&
               std::get<std::int64_t>(result->rows[0].values[0]) == 20,
           "WHERE evaluates arithmetic comparison operands");

    const auto column_comparison = database.execute(
        "SELECT price FROM items WHERE price > quantity;");
    expect(column_comparison && column_comparison->rows.size() == 2,
           "WHERE compares two bound columns");
}

void test_qualified_column_queries() {
    Database database;
    expect(database.execute(
               "CREATE TABLE users(id INT, name TEXT);").has_value() &&
               database.execute(
                   "INSERT INTO users VALUES (2, 'Bob');").has_value() &&
               database.execute(
                   "INSERT INTO users VALUES (1, 'Alice');").has_value(),
           "qualified query test data is created");

    const auto result = database.execute(
        "SELECT users.name AS display_name, users.id + 10 AS score "
        "FROM users WHERE USERS.id >= 1 ORDER BY Users.name;");
    expect(result && result->column_names ==
                         std::vector<std::string>{"display_name", "score"} &&
               result->rows.size() == 2,
           "qualified columns execute across SELECT clauses");
    if (result && result->rows.size() == 2) {
        expect(std::get<std::string>(result->rows[0].values[0]) == "Alice" &&
                   std::get<std::int64_t>(result->rows[0].values[1]) == 11 &&
                   std::get<std::string>(result->rows[1].values[0]) == "Bob" &&
                   std::get<std::int64_t>(result->rows[1].values[1]) == 12,
               "qualified columns preserve filtering, sorting, and arithmetic");
    }

    const auto wrong_table =
        database.execute("SELECT accounts.id FROM users;");
    expect(!wrong_table &&
               std::holds_alternative<BindError>(wrong_table.error()) &&
               std::get<BindError>(wrong_table.error()).code ==
                   BindErrorCode::table_not_found,
           "unknown query qualifier is a recoverable bind error");

    const auto aliased = database.execute(
        "SELECT u.name, COUNT(*) AS rows FROM users AS u "
        "WHERE u.id >= 1 GROUP BY u.name ORDER BY u.name;");
    expect(aliased && aliased->rows.size() == 2 &&
               std::get<std::string>(aliased->rows[0].values[0]) ==
                   "Alice" &&
               std::get<std::int64_t>(aliased->rows[0].values[1]) == 1,
           "explicit table aliases execute across grouped SELECT clauses");

    const auto implicit_alias =
        database.execute("SELECT u.name FROM users u ORDER BY u.name;");
    expect(implicit_alias && implicit_alias->rows.size() == 2,
           "implicit table aliases execute successfully");

    const auto hidden_name =
        database.execute("SELECT users.id FROM users AS u;");
    expect(!hidden_name &&
               std::holds_alternative<BindError>(hidden_name.error()) &&
               std::get<BindError>(hidden_name.error()).code ==
                   BindErrorCode::table_not_found,
           "an alias hides the original table qualifier during execution");
}

void test_join_queries() {
    Database database;
    expect(database.execute(
               "CREATE TABLE users(id INT, name TEXT);").has_value() &&
               database.execute(
                   "CREATE TABLE orders(id INT, user_id INT, item TEXT);")
                   .has_value() &&
               database.execute(
                   "CREATE TABLE shipments(order_id INT, status TEXT);")
                   .has_value(),
           "JOIN test tables are created");
    expect(database.execute(
               "INSERT INTO users VALUES (1, 'Alice');").has_value() &&
               database.execute(
                   "INSERT INTO users VALUES (2, 'Bob');").has_value() &&
               database.execute(
                   "INSERT INTO orders VALUES (10, 2, 'Book');")
                   .has_value() &&
               database.execute(
                   "INSERT INTO orders VALUES (11, 1, 'Pen');")
                   .has_value() &&
               database.execute(
                   "INSERT INTO orders VALUES (12, 2, 'Bag');")
                   .has_value() &&
               database.execute(
                   "INSERT INTO shipments VALUES (10, 'sent');")
                   .has_value() &&
               database.execute(
                   "INSERT INTO shipments VALUES (11, 'waiting');")
                   .has_value(),
           "JOIN test rows are inserted");

    const auto joined = database.execute(
        "SELECT u.name, o.item FROM users u "
        "JOIN orders o ON u.id = o.user_id "
        "WHERE o.id >= 10 ORDER BY o.item;");
    expect(joined && joined->rows.size() == 3 &&
               std::get<std::string>(joined->rows[0].values[0]) == "Bob" &&
               std::get<std::string>(joined->rows[0].values[1]) == "Bag" &&
               std::get<std::string>(joined->rows[2].values[0]) == "Alice",
           "JOIN executes ON, WHERE, projection, and ORDER BY");

    const auto chained = database.execute(
        "SELECT u.name, o.item, s.status FROM users u "
        "JOIN orders o ON u.id = o.user_id "
        "JOIN shipments s ON o.id = s.order_id ORDER BY o.id;");
    expect(chained && chained->rows.size() == 2 &&
               std::get<std::string>(chained->rows[0].values[0]) == "Bob" &&
               std::get<std::string>(chained->rows[0].values[2]) == "sent" &&
               std::get<std::string>(chained->rows[1].values[0]) == "Alice",
           "chained JOIN clauses use cumulative column offsets");

    const auto grouped = database.execute(
        "SELECT u.name, COUNT(*) FROM users u "
        "JOIN orders o ON u.id = o.user_id "
        "GROUP BY u.name ORDER BY u.name;");
    expect(grouped && grouped->rows.size() == 2 &&
               std::get<std::string>(grouped->rows[0].values[0]) == "Alice" &&
               std::get<std::int64_t>(grouped->rows[1].values[1]) == 2,
           "joined rows feed GROUP BY and aggregate execution");

    const auto self_join = database.execute(
        "SELECT a.name, b.name FROM users a "
        "JOIN users b ON a.id = b.id ORDER BY a.id;");
    expect(self_join && self_join->rows.size() == 2,
           "self-join receives independent inputs for repeated table scans");

    const auto explained = database.execute(
        "EXPLAIN SELECT u.name FROM users u "
        "JOIN orders o ON u.id = o.user_id;");
    expect(explained && explained->rows.size() == 4 &&
               std::get<std::string>(explained->rows[1].values[0]).find(
                   "Nested Loop Join") != std::string::npos,
           "EXPLAIN exposes nested-loop join with two scan children");

    const auto ambiguous = database.execute(
        "SELECT id FROM users u JOIN orders o ON u.id = o.user_id;");
    expect(!ambiguous &&
               std::holds_alternative<BindError>(ambiguous.error()) &&
               std::get<BindError>(ambiguous.error()).code ==
                   BindErrorCode::ambiguous_column,
           "JOIN reports ambiguous unqualified columns");
}

void test_delete() {
    Database database;
    expect(database.execute(
               "CREATE TABLE users(id INT, name TEXT);").has_value(),
           "DELETE test table is created");
    expect(database.execute(
               "INSERT INTO users VALUES (1, 'Alice');").has_value() &&
               database.execute(
                   "INSERT INTO users VALUES (2, 'Bob');").has_value() &&
               database.execute(
                   "INSERT INTO users VALUES (3, NULL);").has_value(),
           "DELETE test rows are inserted");

    const auto filtered = database.execute(
        "DELETE FROM users WHERE id = 1 OR name IS NULL;");
    expect(filtered && filtered->rows.empty() &&
               filtered->rows_affected == 2,
           "DELETE reports rows matching its predicate");

    const auto remaining = database.execute("SELECT id FROM users;");
    expect(remaining && remaining->rows.size() == 1 &&
               std::get<std::int64_t>(remaining->rows[0].values[0]) == 2,
           "DELETE removes only matching rows");

    const auto no_match =
        database.execute("DELETE FROM users WHERE id = 99;");
    expect(no_match && no_match->rows_affected == 0,
           "DELETE reports zero when no row matches");

    const auto all = database.execute("DELETE FROM users;");
    expect(all && all->rows_affected == 1,
           "DELETE without WHERE removes every row");
    const auto empty = database.execute("SELECT * FROM users;");
    expect(empty && empty->rows.empty(),
           "table is empty after unfiltered DELETE");

    const auto missing = database.execute("DELETE FROM missing;");
    expect(!missing && std::holds_alternative<BindError>(missing.error()) &&
               std::get<BindError>(missing.error()).code ==
                   BindErrorCode::table_not_found,
           "DELETE rejects an unknown table");
    const auto wrong_type =
        database.execute("DELETE FROM users WHERE id = 'one';");
    expect(!wrong_type &&
               std::holds_alternative<BindError>(wrong_type.error()) &&
               std::get<BindError>(wrong_type.error()).code ==
                   BindErrorCode::type_mismatch,
           "DELETE rejects an incompatible predicate value");
}

void test_order_by() {
    Database database;
    expect(database.execute(
               "CREATE TABLE users(id INT, name TEXT);").has_value(),
           "ORDER BY test table is created");
    expect(database.execute(
               "INSERT INTO users VALUES (2, 'Bob');").has_value() &&
               database.execute(
                   "INSERT INTO users VALUES (1, 'Carol');").has_value() &&
               database.execute(
                   "INSERT INTO users VALUES (2, 'Alice');").has_value() &&
               database.execute(
                   "INSERT INTO users VALUES (3, NULL);").has_value(),
           "ORDER BY test rows are inserted");

    const auto ordered = database.execute(
        "SELECT name FROM users ORDER BY id ASC, name DESC;");
    expect(ordered && ordered->rows.size() == 4 &&
               std::get<std::string>(ordered->rows[0].values[0]) == "Carol" &&
               std::get<std::string>(ordered->rows[1].values[0]) == "Bob" &&
               std::get<std::string>(ordered->rows[2].values[0]) == "Alice" &&
               std::holds_alternative<NullValue>(ordered->rows[3].values[0]),
           "ORDER BY sorts by multiple columns before projection");

    const auto filtered = database.execute(
        "SELECT * FROM users WHERE id = 2 ORDER BY name;");
    expect(filtered && filtered->rows.size() == 2 &&
               std::get<std::string>(filtered->rows[0].values[1]) == "Alice" &&
               std::get<std::string>(filtered->rows[1].values[1]) == "Bob",
           "wildcard SELECT applies filtering before ordering");

    const auto descending =
        database.execute("SELECT name FROM users ORDER BY name DESC;");
    expect(descending && descending->rows.size() == 4 &&
               std::holds_alternative<NullValue>(
                   descending->rows.front().values.front()),
           "descending ORDER BY places NULL before non-NULL values");

    const auto missing =
        database.execute("SELECT name FROM users ORDER BY missing;");
    expect(!missing && std::holds_alternative<BindError>(missing.error()) &&
               std::get<BindError>(missing.error()).code ==
                   BindErrorCode::column_not_found,
           "ORDER BY rejects an unknown column");
}

void test_update() {
    Database database;
    expect(database.execute(
               "CREATE TABLE users(id INT, name TEXT);").has_value(),
           "UPDATE test table is created");
    expect(database.execute(
               "INSERT INTO users VALUES (1, 'Alice');").has_value() &&
               database.execute(
                   "INSERT INTO users VALUES (2, 'Bob');").has_value() &&
               database.execute(
                   "INSERT INTO users VALUES (3, NULL);").has_value(),
           "UPDATE test rows are inserted");

    const auto filtered = database.execute(
        "UPDATE users SET name = 'updated', id = 10 "
        "WHERE id = 1 OR name IS NULL;");
    expect(filtered && filtered->rows.empty() &&
               filtered->rows_affected == 2,
           "UPDATE reports rows matching its predicate");

    const auto selected = database.execute(
        "SELECT id, name FROM users WHERE name = 'updated';");
    expect(selected && selected->rows.size() == 2 &&
               std::get<std::int64_t>(selected->rows[0].values[0]) == 10 &&
               std::get<std::int64_t>(selected->rows[1].values[0]) == 10,
           "UPDATE changes every assigned column on matching rows");

    const auto no_match =
        database.execute("UPDATE users SET name = 'none' WHERE id = 99;");
    expect(no_match && no_match->rows_affected == 0,
           "UPDATE reports zero when no row matches");

    const auto all = database.execute("UPDATE users SET name = NULL;");
    expect(all && all->rows_affected == 3,
           "UPDATE without WHERE changes every row");
    const auto null_rows =
        database.execute("SELECT id FROM users WHERE name IS NULL;");
    expect(null_rows && null_rows->rows.size() == 3,
           "unfiltered UPDATE stores NULL assignments");

    const auto missing =
        database.execute("UPDATE missing SET name = 'value';");
    expect(!missing && std::holds_alternative<BindError>(missing.error()) &&
               std::get<BindError>(missing.error()).code ==
                   BindErrorCode::table_not_found,
           "UPDATE rejects an unknown table");
    const auto wrong_type =
        database.execute("UPDATE users SET id = 'one';");
    expect(!wrong_type &&
               std::holds_alternative<BindError>(wrong_type.error()) &&
               std::get<BindError>(wrong_type.error()).code ==
                   BindErrorCode::type_mismatch,
           "UPDATE rejects an incompatible assignment value");
}

void test_transaction_lifecycle() {
    Database database;

    const auto commit_without_transaction = database.commit_transaction();
    expect(!commit_without_transaction &&
               std::holds_alternative<TransactionExecutionError>(
                   commit_without_transaction.error()) &&
               std::get<TransactionExecutionError>(
                   commit_without_transaction.error()).code ==
                   TransactionExecutionErrorCode::no_active_transaction,
           "COMMIT rejects a missing transaction");

    const auto rollback_without_transaction = database.rollback_transaction();
    expect(!rollback_without_transaction &&
               std::holds_alternative<TransactionExecutionError>(
                   rollback_without_transaction.error()) &&
               std::get<TransactionExecutionError>(
                   rollback_without_transaction.error()).code ==
                   TransactionExecutionErrorCode::no_active_transaction,
           "ROLLBACK rejects a missing transaction");

    expect(database.begin_transaction().has_value() &&
               database.has_active_transaction(),
           "BEGIN starts an explicit transaction");

    const auto nested_begin = database.begin_transaction();
    expect(!nested_begin &&
               std::holds_alternative<TransactionExecutionError>(
                   nested_begin.error()) &&
               std::get<TransactionExecutionError>(nested_begin.error()).code ==
                   TransactionExecutionErrorCode::transaction_already_active,
           "BEGIN rejects a nested transaction");

    expect(database.rollback_transaction().has_value() &&
               !database.has_active_transaction(),
           "ROLLBACK ends an explicit transaction");
}

void test_sql_transaction_control() {
    Database database;
    expect(database.execute("CREATE TABLE items(id INT);").has_value(),
           "SQL transaction table is created");

    const auto missing = database.execute("COMMIT;");
    expect(!missing &&
               std::holds_alternative<TransactionExecutionError>(
                   missing.error()) &&
               std::get<TransactionExecutionError>(missing.error()).code ==
                   TransactionExecutionErrorCode::no_active_transaction,
           "SQL COMMIT rejects a missing transaction");

    expect(database.execute("BEGIN;").has_value() &&
               database.execute("INSERT INTO items VALUES (1);").has_value() &&
               database.execute("ROLLBACK;").has_value(),
           "SQL ROLLBACK executes an explicit transaction");
    const auto rolled_back = database.execute("SELECT id FROM items;");
    expect(rolled_back && rolled_back->rows.empty(),
           "SQL ROLLBACK reverses transaction writes");

    expect(database.execute("begin").has_value() &&
               database.execute("INSERT INTO items VALUES (2);").has_value() &&
               database.execute("commit").has_value(),
           "transaction control SQL is case-insensitive");
    const auto committed = database.execute("SELECT id FROM items;");
    expect(committed && committed->rows.size() == 1 &&
               std::get<std::int64_t>(committed->rows[0].values[0]) == 2,
           "SQL COMMIT preserves transaction writes");
}

void test_in_memory_transactions() {
    Database database;
    expect(database.execute(
               "CREATE TABLE accounts(id INT, name TEXT);").has_value() &&
               database.execute(
                   "INSERT INTO accounts VALUES (1, 'original');").has_value(),
           "transaction test data is created through autocommit");

    expect(database.begin_transaction().has_value(),
           "rollback transaction begins");
    expect(database.execute(
               "UPDATE accounts SET name = 'updated' WHERE id = 1;").has_value() &&
               database.execute(
                   "INSERT INTO accounts VALUES (2, 'transient');").has_value() &&
               database.execute(
                   "DELETE FROM accounts WHERE id = 1;").has_value(),
           "transaction applies UPDATE, INSERT, and DELETE");

    const auto visible_inside = database.execute(
        "SELECT id, name FROM accounts ORDER BY id;");
    expect(visible_inside && visible_inside->rows.size() == 1 &&
               std::get<std::int64_t>(visible_inside->rows[0].values[0]) == 2,
           "a transaction reads its own writes");

    expect(database.rollback_transaction().has_value(),
           "mixed transaction rolls back");
    const auto restored = database.execute(
        "SELECT id, name FROM accounts ORDER BY id;");
    expect(restored && restored->rows.size() == 1 &&
               std::get<std::int64_t>(restored->rows[0].values[0]) == 1 &&
               std::get<std::string>(restored->rows[0].values[1]) == "original",
           "ROLLBACK restores the state before all mutations");

    expect(database.begin_transaction().has_value() &&
               database.execute(
                   "INSERT INTO accounts VALUES (3, 'committed');").has_value() &&
               database.commit_transaction().has_value(),
           "explicit transaction commits");
    const auto committed = database.execute(
        "SELECT name FROM accounts WHERE id = 3;");
    expect(committed && committed->rows.size() == 1 &&
               std::get<std::string>(committed->rows[0].values[0]) == "committed",
           "COMMIT preserves transaction writes");
}

void test_transaction_statement_error() {
    Database database;
    expect(database.execute("CREATE TABLE items(id INT);").has_value() &&
               database.begin_transaction().has_value() &&
               database.execute("INSERT INTO items VALUES (1);").has_value(),
           "statement-error transaction is prepared");

    const auto ddl = database.execute("CREATE TABLE forbidden(id INT);");
    expect(!ddl &&
               std::holds_alternative<TransactionExecutionError>(ddl.error()) &&
               std::get<TransactionExecutionError>(ddl.error()).code ==
                   TransactionExecutionErrorCode::ddl_not_supported &&
               database.has_active_transaction(),
           "unsupported transactional DDL leaves the transaction active");

    expect(database.commit_transaction().has_value(),
           "work before a failed statement can still commit");
    const auto selected = database.execute("SELECT id FROM items;");
    expect(selected && selected->rows.size() == 1 &&
               std::get<std::int64_t>(selected->rows[0].values[0]) == 1,
           "a failed statement does not roll back earlier transaction work");
}

void test_persistent_database() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("nessodb-persistent-query-test-" + std::to_string(suffix) +
                       ".mdb");

    {
        auto database = Database::create(path);
        expect(database.has_value(), "persistent database is created");
        if (!database) {
            return;
        }
        expect(database->execute("CREATE TABLE users(id INT, name TEXT);").has_value(),
               "persistent table is created");
        expect(database->execute(
                   "INSERT INTO users VALUES (1, 'Alice');").has_value(),
               "persistent row is inserted");
    }

    {
        auto database = Database::open(path);
        expect(database.has_value(), "persistent database is reopened");
        if (database) {
            const auto selected =
                database->execute("SELECT name FROM users WHERE id = 1;");
            expect(selected && selected->rows.size() == 1 &&
                       selected->rows[0].values.size() == 1 &&
                       std::get<std::string>(selected->rows[0].values[0]) == "Alice",
                   "persistent column is projected after reopening");
            const auto updated =
                database->execute("UPDATE users SET name = 'Updated' WHERE id = 1;");
            expect(updated && updated->rows_affected == 1,
                   "persistent UPDATE reports the changed row");
        }
    }

    {
        auto database = Database::open(path);
        expect(database.has_value(),
               "database is reopened after persistent UPDATE");
        if (database) {
            const auto selected =
                database->execute("SELECT name FROM users WHERE id = 1;");
            expect(selected && selected->rows.size() == 1 &&
                       std::get<std::string>(selected->rows[0].values[0]) ==
                           "Updated",
                   "persistent UPDATE survives reopening");
            const auto deleted =
                database->execute("DELETE FROM users WHERE id = 1;");
            expect(deleted && deleted->rows_affected == 1,
                   "persistent DELETE reports the removed row");
        }
    }

    {
        auto database = Database::open(path);
        expect(database.has_value(),
               "database is reopened after persistent DELETE");
        if (database) {
            const auto selected = database->execute("SELECT * FROM users;");
            expect(selected && selected->rows.empty(),
                   "persistent DELETE survives reopening");
        }
    }

    remove_database_files(path);
}

void test_restart_recovery_precedes_database_validation() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("nessodb-restart-recovery-query-test-" +
                       std::to_string(suffix) + ".mdb");
    struct Cleanup {
        const std::filesystem::path& path;

        ~Cleanup() {
            remove_database_files(path);
        }
    } cleanup{path};

    {
        auto database = Database::create(path);
        expect(database.has_value(), "recovery database is created");
        if (!database) {
            return;
        }
    }

    std::optional<nessodb::storage::PageBuffer> committed_header;
    {
        auto page_file = nessodb::storage::PageFile::open(path);
        auto wal = nessodb::recovery::WriteAheadLog::open(path);
        expect(page_file.has_value() && wal.has_value(),
               "recovery fixtures are opened");
        if (!page_file || !wal) {
            return;
        }
        const auto page_count = page_file->page_count();
        const auto before = page_file->read_page(nessodb::common::PageId{0});
        expect(page_count.has_value() && before.has_value(),
               "database header is read before corruption");
        if (!page_count || !before) {
            return;
        }
        expect(wal->begin(nessodb::common::TransactionId{1000}, *page_count)
                   .has_value(),
               "recovery transaction begins");
        auto logged = wal->log_page_update(
            nessodb::common::PageId{0}, *before, *before);
        expect(logged.has_value(), "committed header image is logged");
        if (!logged) {
            return;
        }
        committed_header = logged->page;
        expect(wal->commit(nessodb::common::TransactionId{1000}).has_value(),
               "recovery transaction commits");

        auto corrupted = logged->page;
        corrupted[128] = std::byte{0x7f};
        expect(page_file->write_page(nessodb::common::PageId{0}, corrupted)
                       .has_value() &&
                   page_file->flush().has_value(),
               "data page is corrupted after the committed WAL record");
    }

    {
        auto database = Database::open(path);
        expect(database.has_value(),
               "restart recovery repairs the page before database validation");
    }
    {
        auto page_file = nessodb::storage::PageFile::open(path);
        expect(page_file.has_value(),
               "recovered database page file reopens");
        if (page_file) {
            const auto recovered =
                page_file->read_page(nessodb::common::PageId{0});
            expect(recovered && committed_header &&
                       *recovered == *committed_header,
                   "automatic recovery writes the committed page image");
        }
    }
}

void test_open_missing_database_does_not_create_wal() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("nessodb-missing-recovery-query-test-" +
                       std::to_string(suffix) + ".mdb");

    const auto opened = Database::open(path);
    expect(!opened, "missing database is rejected before WAL creation");
    expect(!std::filesystem::exists(nessodb::recovery::wal_path_for(path)),
           "opening a missing database leaves no WAL file behind");
}

void test_persistent_transaction_boundaries_are_logged() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("nessodb-transaction-wal-query-test-" +
                       std::to_string(suffix) + ".mdb");
    struct Cleanup {
        const std::filesystem::path& path;

        ~Cleanup() {
            remove_database_files(path);
        }
    } cleanup{path};
    std::uintmax_t empty_wal_size{};

    {
        auto database = Database::create(path);
        expect(database.has_value(), "transaction WAL database is created");
        if (!database) {
            return;
        }
        const auto wal_path = nessodb::recovery::wal_path_for(path);
        empty_wal_size = std::filesystem::file_size(wal_path);

        expect(database->begin_transaction().has_value(),
               "persistent transaction begins");
        const auto begun_size = std::filesystem::file_size(wal_path);
        expect(begun_size > empty_wal_size,
               "BEGIN appends a durable WAL record");
        expect(database->commit_transaction().has_value(),
               "persistent transaction commits");
        const auto committed_size = std::filesystem::file_size(wal_path);
        expect(committed_size > begun_size,
               "COMMIT appends a durable WAL record");

        expect(database->begin_transaction().has_value(),
               "second persistent transaction begins");
        const auto second_begin_size = std::filesystem::file_size(wal_path);
        expect(database->rollback_transaction().has_value(),
               "second persistent transaction rolls back");
        expect(std::filesystem::file_size(wal_path) > second_begin_size,
               "ROLLBACK appends a durable abort record");
    }

    {
        auto database = Database::open(path);
        expect(database.has_value(),
               "database reopens after logged transaction boundaries");
        expect(std::filesystem::file_size(
                   nessodb::recovery::wal_path_for(path)) == empty_wal_size,
               "restart recovery checkpoints completed transactions");
    }
}

void test_primary_key_queries() {
    Database memory;
    expect(memory.execute(
               "CREATE TABLE users(id INT PRIMARY KEY, name TEXT);")
               .has_value() &&
               memory.execute(
                   "INSERT INTO users VALUES (1, 'first');")
               .has_value() &&
               memory.execute(
                   "INSERT INTO users VALUES (2, 'second');")
               .has_value(),
           "in-memory primary key table and rows are created");

    const auto duplicate = memory.execute(
        "INSERT INTO users VALUES (1, 'duplicate');");
    const auto* duplicate_storage = duplicate
        ? nullptr
        : std::get_if<StorageError>(&duplicate.error());
    expect(duplicate_storage != nullptr &&
               std::get_if<nessodb::storage::StorageManagerErrorCode>(
                   &duplicate_storage->cause) != nullptr &&
               std::get<nessodb::storage::StorageManagerErrorCode>(
                   duplicate_storage->cause) ==
                   nessodb::storage::StorageManagerErrorCode::
                       unique_constraint_violation,
           "in-memory primary key rejects duplicates");

    const auto null_key = memory.execute(
        "INSERT INTO users VALUES (NULL, 'null');");
    const auto* null_storage = null_key
        ? nullptr
        : std::get_if<StorageError>(&null_key.error());
    expect(null_storage != nullptr &&
               std::get_if<nessodb::storage::StorageManagerErrorCode>(
                   &null_storage->cause) != nullptr &&
               std::get<nessodb::storage::StorageManagerErrorCode>(
                   null_storage->cause) ==
                   nessodb::storage::StorageManagerErrorCode::primary_key_null,
           "in-memory primary key rejects NULL");

    const auto conflicting_update = memory.execute(
        "UPDATE users SET id = 1 WHERE id = 2;");
    expect(!conflicting_update,
           "in-memory primary key rejects a conflicting update");
    const auto unchanged = memory.execute(
        "SELECT id FROM users ORDER BY id;");
    expect(unchanged && unchanged->rows.size() == 2 &&
               std::get<std::int64_t>(unchanged->rows[0].values[0]) == 1 &&
               std::get<std::int64_t>(unchanged->rows[1].values[0]) == 2,
           "failed primary key update leaves rows unchanged");

    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("nessodb-primary-key-query-test-" +
                       std::to_string(suffix) + ".mdb");
    {
        auto database = Database::create(path);
        expect(database && database->execute(
                   "CREATE TABLE accounts(id INT PRIMARY KEY, name TEXT);") &&
                   database->execute(
                       "INSERT INTO accounts VALUES (7, 'Ada');"),
               "persistent primary key data is created");
        if (database) {
            expect(database->begin_transaction() &&
                       database->execute(
                           "UPDATE accounts SET id = 8 WHERE id = 7;") &&
                       database->execute(
                           "INSERT INTO accounts VALUES (9, 'temporary');") &&
                       database->execute(
                           "DELETE FROM accounts WHERE id = 8;") &&
                       database->rollback_transaction(),
                   "primary key mutations roll back together");
            const auto restored = database->execute(
                "SELECT id FROM accounts WHERE id = 7;");
            const auto removed = database->execute(
                "SELECT id FROM accounts WHERE id = 9;");
            expect(restored && restored->rows.size() == 1 &&
                       removed && removed->rows.empty(),
                   "rollback restores every primary key index entry");
            const auto explain = database->execute(
                "EXPLAIN SELECT name FROM accounts WHERE id = 7;");
            expect(explain && !explain->rows.empty() &&
                       std::get<std::string>(
                           explain->rows.back().values[0]) ==
                           "    Index Scan (table_id=1, column=0)",
                   "EXPLAIN selects the primary key index scan");
        }
    }
    {
        auto database = Database::open(path);
        expect(database.has_value(),
               "persistent primary key database reopens");
        if (database) {
            const auto selected = database->execute(
                "SELECT name FROM accounts WHERE 7 = id;");
            expect(selected && selected->rows.size() == 1 &&
                       std::get<std::string>(
                           selected->rows[0].values[0]) == "Ada",
                   "persistent primary key lookup survives reopening");
        }
    }
    remove_database_files(path);
}

void test_persistent_transaction_rollback() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("nessodb-persistent-transaction-test-" +
                       std::to_string(suffix) + ".mdb");

    {
        auto database = Database::create(path);
        expect(database.has_value(), "persistent transaction database is created");
        if (!database) {
            return;
        }
        expect(database->execute(
                   "CREATE TABLE accounts(id INT, name TEXT);").has_value() &&
                   database->execute(
                       "INSERT INTO accounts VALUES (1, 'original');").has_value() &&
                   database->begin_transaction().has_value() &&
                   database->execute(
                       "UPDATE accounts SET name = 'updated' WHERE id = 1;").has_value() &&
                   database->execute(
                       "INSERT INTO accounts VALUES (2, 'transient');").has_value() &&
                   database->execute(
                       "DELETE FROM accounts WHERE id = 1;").has_value() &&
                   database->rollback_transaction().has_value(),
               "persistent mutations roll back");
    }

    {
        auto database = Database::open(path);
        expect(database.has_value(),
               "persistent transaction database reopens after rollback");
        if (database) {
            const auto restored = database->execute(
                "SELECT id, name FROM accounts ORDER BY id;");
            expect(restored && restored->rows.size() == 1 &&
                       std::get<std::int64_t>(restored->rows[0].values[0]) == 1 &&
                       std::get<std::string>(restored->rows[0].values[1]) ==
                           "original",
                   "persistent rollback survives reopening");
        }
    }

    remove_database_files(path);
}

void test_limit() {
    Database database;
    expect(database.execute("CREATE TABLE items(id INT);").has_value() &&
               database.execute("INSERT INTO items VALUES (3);").has_value() &&
               database.execute("INSERT INTO items VALUES (1);").has_value() &&
               database.execute("INSERT INTO items VALUES (2);").has_value(),
           "LIMIT test data is created");

    const auto result =
        database.execute("SELECT id FROM items ORDER BY id LIMIT 2;");
    expect(result && result->rows.size() == 2 &&
               std::get<std::int64_t>(result->rows[0].values[0]) == 1 &&
               std::get<std::int64_t>(result->rows[1].values[0]) == 2,
           "LIMIT is applied after ordering");

    const auto empty = database.execute("SELECT * FROM items LIMIT 0;");
    expect(empty && empty->rows.empty(), "LIMIT zero returns no rows");

    const auto offset =
        database.execute("SELECT id FROM items ORDER BY id LIMIT 1 OFFSET 1;");
    expect(offset && offset->rows.size() == 1 &&
               std::get<std::int64_t>(offset->rows[0].values[0]) == 2,
           "OFFSET skips ordered rows before applying LIMIT");

    const auto offset_only =
        database.execute("SELECT id FROM items ORDER BY id OFFSET 2;");
    expect(offset_only && offset_only->rows.size() == 1 &&
               std::get<std::int64_t>(
                   offset_only->rows[0].values[0]) == 3,
           "OFFSET can be used without LIMIT");
}

void test_distinct() {
    Database database;
    expect(database.execute(
               "CREATE TABLE values_table(value INT);").has_value() &&
               database.execute(
                   "INSERT INTO values_table VALUES (2);").has_value() &&
               database.execute(
                   "INSERT INTO values_table VALUES (1);").has_value() &&
               database.execute(
                   "INSERT INTO values_table VALUES (2);").has_value() &&
               database.execute(
                   "INSERT INTO values_table VALUES (NULL);").has_value() &&
               database.execute(
                   "INSERT INTO values_table VALUES (NULL);").has_value(),
           "DISTINCT test data is created");

    const auto result = database.execute(
        "SELECT DISTINCT value FROM values_table ORDER BY value LIMIT 2;");
    expect(result && result->rows.size() == 2 &&
               std::get<std::int64_t>(result->rows[0].values[0]) == 1 &&
               std::get<std::int64_t>(result->rows[1].values[0]) == 2,
           "DISTINCT runs after projection and before LIMIT");

    const auto all =
        database.execute("SELECT DISTINCT * FROM values_table;");
    expect(all && all->rows.size() == 3,
           "DISTINCT wildcard removes duplicate rows including NULL");
}

void test_explain() {
    Database database;
    expect(database.execute(
               "CREATE TABLE users(id INT, name TEXT);").has_value(),
           "EXPLAIN test table is created");

    const auto result = database.execute(
        "EXPLAIN SELECT name FROM users WHERE id >= 1 "
        "ORDER BY name DESC LIMIT 2;");
    expect(result && result->rows.size() == 5,
           "EXPLAIN returns one row for each physical operator");
    expect(result && result->column_names ==
                         std::vector<std::string>{"QUERY PLAN"},
           "EXPLAIN exposes a stable result column name");
    if (result && result->rows.size() == 5) {
        const std::vector<std::string> expected{
            "Limit (limit=2, offset=0)",
            "  Projection (expressions=1)",
            "    In-Memory Sort (keys=column[1] DESC)",
            "      Filter",
            "        Sequential Scan (table_id=1)",
        };
        for (std::size_t index = 0; index < expected.size(); ++index) {
            expect(result->rows[index].values.size() == 1 &&
                       std::get<std::string>(
                           result->rows[index].values[0]) == expected[index],
                   "EXPLAIN returns the formatted physical plan");
        }
    }

    const auto missing = database.execute(
        "EXPLAIN SELECT * FROM missing;");
    expect(!missing && std::holds_alternative<BindError>(missing.error()),
           "EXPLAIN binds its SELECT statement");
}

void test_arithmetic_queries() {
    const auto result = execute_query(
        "SELECT 1 + 2 * 3, -(10 / 2), 20 / 5 / 2, NULL + 4;");
    expect(result && result->rows.size() == 1 &&
               result->rows[0].values.size() == 4,
           "arithmetic query returns every projection");
    if (result && result->rows.size() == 1 &&
        result->rows[0].values.size() == 4) {
        expect(std::get<std::int64_t>(result->rows[0].values[0]) == 7,
               "arithmetic uses multiplicative precedence");
        expect(std::get<std::int64_t>(result->rows[0].values[1]) == -5,
               "grouping and unary minus are executed");
        expect(std::get<std::int64_t>(result->rows[0].values[2]) == 2,
               "binary arithmetic is left-associative");
        expect(std::holds_alternative<NullValue>(
                   result->rows[0].values[3]),
               "arithmetic propagates NULL");
    }

    Database database;
    expect(database.execute("CREATE TABLE numbers(value INT);").has_value() &&
               database.execute(
                   "INSERT INTO numbers VALUES (21);").has_value(),
           "arithmetic column test data is created");
    const auto column = database.execute(
        "SELECT value * 2 FROM numbers;");
    expect(column && column->rows.size() == 1 &&
               std::get<std::int64_t>(column->rows[0].values[0]) == 42,
           "arithmetic evaluates bound column references");

    const auto mismatch = execute_query("SELECT 'text' + 1;");
    expect(!mismatch && std::holds_alternative<BindError>(mismatch.error()) &&
               std::get<BindError>(mismatch.error()).code ==
                   BindErrorCode::type_mismatch,
           "arithmetic rejects text operands during binding");

    const auto division = execute_query("SELECT 10 / 0;");
    const auto* division_error = division
                                     ? nullptr
                                     : std::get_if<ExecutionError>(
                                           &division.error());
    expect(division_error &&
               division_error->code ==
                   ExecutionErrorCode::division_by_zero &&
               division_error->location ==
                   nessodb::sql::SourceLocation{10, 1, 11},
           "division by zero reports the operator location");

    const auto overflow =
        execute_query("SELECT 9223372036854775807 + 1;");
    const auto* overflow_error = overflow
                                     ? nullptr
                                     : std::get_if<ExecutionError>(
                                           &overflow.error());
    expect(overflow_error &&
               overflow_error->code ==
                   ExecutionErrorCode::integer_overflow,
           "arithmetic overflow is a recoverable execution error");

    const auto minimum = execute_query("SELECT -9223372036854775808;");
    expect(minimum && minimum->rows.size() == 1 &&
               std::get<std::int64_t>(minimum->rows[0].values[0]) ==
                   std::numeric_limits<std::int64_t>::min(),
           "unary minus represents the minimum signed integer");

    const auto below_minimum =
        execute_query("SELECT -9223372036854775809;");
    expect(!below_minimum &&
               std::holds_alternative<BindError>(below_minimum.error()) &&
               std::get<BindError>(below_minimum.error()).code ==
                   BindErrorCode::integer_out_of_range,
           "magnitude below the minimum integer is rejected");

    const auto negated_minimum =
        execute_query("SELECT --9223372036854775808;");
    expect(!negated_minimum &&
               std::holds_alternative<ExecutionError>(
                   negated_minimum.error()) &&
               std::get<ExecutionError>(negated_minimum.error()).code ==
                   ExecutionErrorCode::integer_overflow,
           "negating the minimum integer reports overflow");
}

}  // namespace

int main() {
    test_query_result();
    test_query_errors();
    test_create_table();
    test_insert();
    test_select_from();
    test_order_by();
    test_limit();
    test_distinct();
    test_explain();
    test_arithmetic_queries();
    test_count_queries();
    test_group_by_queries();
    test_predicate_expression_queries();
    test_qualified_column_queries();
    test_join_queries();
    test_delete();
    test_update();
    test_transaction_lifecycle();
    test_sql_transaction_control();
    test_in_memory_transactions();
    test_transaction_statement_error();
    test_persistent_database();
    test_restart_recovery_precedes_database_validation();
    test_open_missing_database_does_not_create_wal();
    test_persistent_transaction_boundaries_are_logged();
    test_primary_key_queries();
    test_persistent_transaction_rollback();

    if (failures != 0) {
        std::cerr << failures << " query assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
