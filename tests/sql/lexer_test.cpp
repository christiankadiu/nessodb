#include "sql/lexer.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

using nessodb::sql::Lexer;
using nessodb::sql::LexErrorCode;
using nessodb::sql::SourceLocation;
using nessodb::sql::Token;
using nessodb::sql::TokenType;

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

Token next_token(Lexer& lexer) {
    auto result = lexer.next();
    if (!result) {
        std::cerr << "FAILED: unexpected lexer error\n";
        ++failures;
        return {};
    }
    return *result;
}

void test_empty_input() {
    Lexer lexer{""};
    const Token token = next_token(lexer);

    expect(token.type == TokenType::end_of_input, "empty input produces end token");
    expect(token.location == SourceLocation{0, 1, 1}, "end token location is correct");
}

void test_select_query_prefix() {
    Lexer lexer{"SeLeCt answer_1 42"};

    const Token keyword = next_token(lexer);
    expect(keyword.type == TokenType::select, "SELECT is case-insensitive");
    expect(keyword.lexeme == "SeLeCt", "keyword spelling is preserved");

    const Token identifier = next_token(lexer);
    expect(identifier.type == TokenType::identifier, "identifier is recognized");
    expect(identifier.lexeme == "answer_1", "identifier lexeme is preserved");

    const Token integer = next_token(lexer);
    expect(integer.type == TokenType::integer_literal, "integer is recognized");
    expect(integer.lexeme == "42", "integer lexeme is preserved");

    expect(next_token(lexer).type == TokenType::end_of_input, "input ends cleanly");
}

void test_from_keyword() {
    Lexer lexer{"FROM FrOm fromage"};

    expect(next_token(lexer).type == TokenType::from, "FROM is recognized");
    expect(next_token(lexer).type == TokenType::from, "FROM is case-insensitive");
    expect(next_token(lexer).type == TokenType::identifier,
           "identifier beginning with FROM is preserved");
}

void test_where_equality_tokens() {
    Lexer lexer{"WhErE id = 1"};

    expect(next_token(lexer).type == TokenType::where,
           "WHERE is case-insensitive");
    expect(next_token(lexer).type == TokenType::identifier,
           "predicate column is recognized");
    expect(next_token(lexer).type == TokenType::equal,
           "equality operator is recognized");
    expect(next_token(lexer).type == TokenType::integer_literal,
           "predicate literal is recognized");

    constexpr std::pair<std::string_view, TokenType> operators[] = {
        {"=", TokenType::equal},
        {"!=", TokenType::not_equal},
        {"<", TokenType::less},
        {"<=", TokenType::less_equal},
        {">", TokenType::greater},
        {">=", TokenType::greater_equal},
    };
    for (const auto& [source, type] : operators) {
        Lexer operator_lexer{source};
        expect(next_token(operator_lexer).type == type,
               "comparison operator has expected token type");
    }
}

void test_null_predicate_tokens() {
    Lexer lexer{"IS NOT NULL is null"};
    constexpr TokenType expected[] = {
        TokenType::is,
        TokenType::not_keyword,
        TokenType::null_literal,
        TokenType::is,
        TokenType::null_literal,
        TokenType::end_of_input,
    };
    for (const auto type : expected) {
        expect(next_token(lexer).type == type,
               "NULL predicate token has expected type");
    }
}

void test_and_keyword() {
    Lexer lexer{"AND aNd android"};

    expect(next_token(lexer).type == TokenType::and_keyword,
           "AND is recognized");
    expect(next_token(lexer).type == TokenType::and_keyword,
           "AND is case-insensitive");
    expect(next_token(lexer).type == TokenType::identifier,
           "identifier beginning with AND is preserved");
}

void test_or_keyword() {
    Lexer lexer{"OR oR oracle"};

    expect(next_token(lexer).type == TokenType::or_keyword,
           "OR is recognized");
    expect(next_token(lexer).type == TokenType::or_keyword,
           "OR is case-insensitive");
    expect(next_token(lexer).type == TokenType::identifier,
           "identifier beginning with OR is preserved");
}

void test_delete_tokens() {
    Lexer lexer{"DELETE FROM users"};

    expect(next_token(lexer).type == TokenType::delete_keyword,
           "DELETE is recognized");
    expect(next_token(lexer).type == TokenType::from,
           "DELETE FROM contains FROM keyword");
    expect(next_token(lexer).type == TokenType::identifier,
           "DELETE table name is recognized");
}

void test_update_tokens() {
    Lexer lexer{"UPDATE users SET name = 'Alice'"};

    expect(next_token(lexer).type == TokenType::update,
           "UPDATE is recognized");
    expect(next_token(lexer).type == TokenType::identifier,
           "UPDATE table name is recognized");
    expect(next_token(lexer).type == TokenType::set,
           "SET is recognized");
}

void test_transaction_control_tokens() {
    Lexer lexer{"BeGiN COMMIT rollback"};

    expect(next_token(lexer).type == TokenType::begin,
           "BEGIN is case-insensitive");
    expect(next_token(lexer).type == TokenType::commit,
           "COMMIT is recognized");
    expect(next_token(lexer).type == TokenType::rollback,
           "ROLLBACK is recognized");
}

void test_order_by_tokens() {
    Lexer lexer{"ORDER BY name ASC, id DESC"};
    constexpr TokenType expected[]{
        TokenType::order,
        TokenType::by,
        TokenType::identifier,
        TokenType::asc,
        TokenType::comma,
        TokenType::identifier,
        TokenType::desc,
    };
    for (const TokenType type : expected) {
        expect(next_token(lexer).type == type,
               "ORDER BY token has expected type");
    }
}

void test_group_by_tokens() {
    Lexer lexer{"GROUP BY category, region"};
    constexpr TokenType expected[]{
        TokenType::group,
        TokenType::by,
        TokenType::identifier,
        TokenType::comma,
        TokenType::identifier,
    };
    for (const TokenType type : expected) {
        expect(next_token(lexer).type == type,
               "GROUP BY token has expected type");
    }
}

void test_join_tokens() {
    Lexer lexer{"JOIN orders AS o ON u.id = o.user_id"};
    constexpr TokenType expected[]{
        TokenType::join, TokenType::identifier,
        TokenType::as_keyword, TokenType::identifier,
        TokenType::on, TokenType::identifier, TokenType::dot,
        TokenType::identifier, TokenType::equal,
        TokenType::identifier, TokenType::dot, TokenType::identifier,
    };
    for (const TokenType type : expected) {
        expect(next_token(lexer).type == type,
               "JOIN token has expected type");
    }
}

void test_limit_token() {
    Lexer lexer{"LIMIT 10 OFFSET 2"};
    expect(next_token(lexer).type == TokenType::limit,
           "LIMIT is recognized");
    expect(next_token(lexer).type == TokenType::integer_literal,
           "LIMIT count is recognized");
    expect(next_token(lexer).type == TokenType::offset,
           "OFFSET is recognized");
    expect(next_token(lexer).type == TokenType::integer_literal,
           "OFFSET count is recognized");
}

void test_distinct_token() {
    Lexer lexer{"SELECT DISTINCT name"};
    expect(next_token(lexer).type == TokenType::select,
           "SELECT is recognized before DISTINCT");
    expect(next_token(lexer).type == TokenType::distinct,
           "DISTINCT is recognized");
}

void test_alias_token() {
    Lexer lexer{"SELECT value AS result"};
    expect(next_token(lexer).type == TokenType::select,
           "SELECT is recognized before an alias");
    expect(next_token(lexer).type == TokenType::identifier,
           "aliased expression is recognized");
    expect(next_token(lexer).type == TokenType::as_keyword,
           "AS is recognized");
    expect(next_token(lexer).type == TokenType::identifier,
           "alias name is recognized");
}

void test_qualified_column_tokens() {
    Lexer lexer{"users.name"};
    expect(next_token(lexer).type == TokenType::identifier,
           "qualified column starts with an identifier");
    expect(next_token(lexer).type == TokenType::dot,
           "qualified column contains a dot");
    expect(next_token(lexer).type == TokenType::identifier,
           "qualified column ends with an identifier");
}

void test_explain_token() {
    Lexer lexer{"ExPlAiN SELECT 1"};
    expect(next_token(lexer).type == TokenType::explain,
           "EXPLAIN is case-insensitive");
    expect(next_token(lexer).type == TokenType::select,
           "EXPLAIN is followed by SELECT");
}

void test_wildcard_projection() {
    Lexer lexer{"SELECT * FROM users;"};
    constexpr TokenType expected[] = {
        TokenType::select,
        TokenType::star,
        TokenType::from,
        TokenType::identifier,
        TokenType::semicolon,
        TokenType::end_of_input,
    };

    for (const TokenType type : expected) {
        expect(next_token(lexer).type == type, "wildcard SELECT token has expected type");
    }
}

void test_arithmetic_operators() {
    Lexer lexer{"+ - * /"};
    constexpr TokenType expected[] = {
        TokenType::plus,
        TokenType::minus,
        TokenType::star,
        TokenType::slash,
        TokenType::end_of_input,
    };

    for (const TokenType type : expected) {
        expect(next_token(lexer).type == type,
               "arithmetic operator has expected token type");
    }
}

void test_locations() {
    Lexer lexer{"SELECT\r\n  value"};
    const Token keyword = next_token(lexer);
    const Token identifier = next_token(lexer);

    expect(keyword.location == SourceLocation{0, 1, 1}, "keyword location is correct");
    expect(identifier.location == SourceLocation{10, 2, 3}, "CRLF location is correct");
}

void test_select_delimiters() {
    Lexer lexer{"SELECT 1, 2;"};

    expect(next_token(lexer).type == TokenType::select, "SELECT is recognized");
    expect(next_token(lexer).type == TokenType::integer_literal, "first integer is recognized");
    expect(next_token(lexer).type == TokenType::comma, "comma is recognized");
    expect(next_token(lexer).type == TokenType::integer_literal, "second integer is recognized");
    expect(next_token(lexer).type == TokenType::semicolon, "semicolon is recognized");
    expect(next_token(lexer).type == TokenType::end_of_input, "input ends after semicolon");
}

void test_string_literals() {
    Lexer lexer{"'Alice' '' 'it''s done'"};

    const Token text = next_token(lexer);
    expect(text.type == TokenType::string_literal, "text literal is recognized");
    expect(text.lexeme == "'Alice'", "text literal lexeme is preserved");

    const Token empty = next_token(lexer);
    expect(empty.type == TokenType::string_literal, "empty text literal is recognized");
    expect(empty.lexeme == "''", "empty text literal lexeme is preserved");

    const Token escaped = next_token(lexer);
    expect(escaped.type == TokenType::string_literal, "escaped quote is recognized");
    expect(escaped.lexeme == "'it''s done'", "escaped quote remains in the lexeme");
}

void test_null_keyword() {
    Lexer lexer{"NULL NuLl nullable"};

    expect(next_token(lexer).type == TokenType::null_literal, "NULL is recognized");
    expect(next_token(lexer).type == TokenType::null_literal, "NULL is case-insensitive");
    expect(next_token(lexer).type == TokenType::identifier,
           "identifier beginning with null is preserved");
}

void test_create_table_tokens() {
    Lexer lexer{"CREATE TABLE users(id INT, name TEXT);"};
    constexpr TokenType expected[] = {
        TokenType::create,
        TokenType::table,
        TokenType::identifier,
        TokenType::left_parenthesis,
        TokenType::identifier,
        TokenType::integer_type,
        TokenType::comma,
        TokenType::identifier,
        TokenType::text_type,
        TokenType::right_parenthesis,
        TokenType::semicolon,
        TokenType::end_of_input,
    };

    for (const TokenType type : expected) {
        expect(next_token(lexer).type == type, "CREATE TABLE token has expected type");
    }
}

void test_insert_tokens() {
    Lexer lexer{"InSeRt INTO users VALUES (1, 'Alice', NULL); insertion"};
    constexpr TokenType expected[] = {
        TokenType::insert,
        TokenType::into,
        TokenType::identifier,
        TokenType::values,
        TokenType::left_parenthesis,
        TokenType::integer_literal,
        TokenType::comma,
        TokenType::string_literal,
        TokenType::comma,
        TokenType::null_literal,
        TokenType::right_parenthesis,
        TokenType::semicolon,
        TokenType::identifier,
        TokenType::end_of_input,
    };

    for (const TokenType type : expected) {
        expect(next_token(lexer).type == type, "INSERT token has expected type");
    }
}

void test_invalid_character() {
    Lexer lexer{"@"};
    const auto result = lexer.next();

    expect(!result, "invalid character produces an error");
    if (!result) {
        expect(result.error().code == LexErrorCode::invalid_character,
               "invalid character has expected error code");
        expect(result.error().character == '@', "error contains invalid character");
        expect(result.error().location == SourceLocation{0, 1, 1},
               "error location is correct");
    }
}

void test_unterminated_string() {
    Lexer lexer{"'unfinished"};
    const auto result = lexer.next();

    expect(!result, "unterminated string produces an error");
    if (!result) {
        expect(result.error().code == LexErrorCode::unterminated_string,
               "unterminated string has expected error code");
        expect(result.error().location == SourceLocation{0, 1, 1},
               "unterminated string points to opening quote");
    }
}

}  // namespace

int main() {
    test_empty_input();
    test_select_query_prefix();
    test_from_keyword();
    test_where_equality_tokens();
    test_null_predicate_tokens();
    test_and_keyword();
    test_or_keyword();
    test_delete_tokens();
    test_update_tokens();
    test_transaction_control_tokens();
    test_order_by_tokens();
    test_group_by_tokens();
    test_join_tokens();
    test_limit_token();
    test_distinct_token();
    test_alias_token();
    test_qualified_column_tokens();
    test_explain_token();
    test_wildcard_projection();
    test_arithmetic_operators();
    test_locations();
    test_select_delimiters();
    test_string_literals();
    test_null_keyword();
    test_create_table_tokens();
    test_insert_tokens();
    test_invalid_character();
    test_unterminated_string();

    if (failures != 0) {
        std::cerr << failures << " lexer assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
