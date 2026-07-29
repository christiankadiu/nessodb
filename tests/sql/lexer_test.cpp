#include "sql/lexer.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using minidb::sql::Lexer;
using minidb::sql::LexErrorCode;
using minidb::sql::SourceLocation;
using minidb::sql::Token;
using minidb::sql::TokenType;

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
    test_locations();
    test_select_delimiters();
    test_string_literals();
    test_null_keyword();
    test_invalid_character();
    test_unterminated_string();

    if (failures != 0) {
        std::cerr << failures << " lexer assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
