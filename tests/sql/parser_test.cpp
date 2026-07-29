#include "sql/parser.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using minidb::sql::ParseErrorCode;
using minidb::sql::Parser;
using minidb::sql::SourceLocation;

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

void test_select_list() {
    Parser parser{"SELECT 1, 22;"};
    const auto result = parser.parse_select_statement();

    expect(result.has_value(), "valid SELECT is parsed");
    if (!result) {
        return;
    }

    expect(result->expressions.size() == 2, "SELECT contains two expressions");
    if (result->expressions.size() == 2) {
        expect(result->expressions[0].text == "1", "first integer text is preserved");
        expect(result->expressions[0].location == SourceLocation{7, 1, 8},
               "first integer location is preserved");
        expect(result->expressions[1].text == "22", "second integer text is preserved");
    }
}

void test_optional_semicolon() {
    Parser parser{"SELECT 7"};
    const auto result = parser.parse_select_statement();

    expect(result.has_value(), "semicolon is optional");
    if (result) {
        expect(result->expressions.size() == 1, "single expression is parsed");
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
    expect_error("SELECT;", ParseErrorCode::expected_integer_literal, SourceLocation{6, 1, 7},
                 "SELECT requires an integer");
    expect_error("SELECT 1,;", ParseErrorCode::expected_integer_literal, SourceLocation{9, 1, 10},
                 "comma must be followed by an integer");
    expect_error("SELECT 1 2", ParseErrorCode::expected_comma_or_end, SourceLocation{9, 1, 10},
                 "expressions require a comma");
    expect_error("SELECT 1; 2", ParseErrorCode::expected_end_of_input, SourceLocation{10, 1, 11},
                 "tokens after semicolon are rejected");
    expect_error("SELECT @", ParseErrorCode::lexical_error, SourceLocation{7, 1, 8},
                 "lexer errors are propagated");
}

}  // namespace

int main() {
    test_select_list();
    test_optional_semicolon();
    test_errors();

    if (failures != 0) {
        std::cerr << failures << " parser assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
