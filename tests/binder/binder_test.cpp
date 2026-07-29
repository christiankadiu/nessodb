#include "binder/binder.hpp"
#include "sql/parser.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <variant>

namespace {

using minidb::binder::BindErrorCode;
using minidb::binder::bind_select_statement;
using minidb::sql::Parser;
using minidb::sql::SourceLocation;
using minidb::types::NullValue;

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

void test_integer_literals() {
    Parser parser{"SELECT 0, 9223372036854775807;"};
    const auto parsed = parser.parse_select_statement();
    expect(parsed.has_value(), "test query is parsed");
    if (!parsed) {
        return;
    }

    const auto bound = bind_select_statement(*parsed);
    expect(bound.has_value(), "integer literals are bound");
    if (!bound) {
        return;
    }

    expect(bound->expressions.size() == 2, "two expressions are bound");
    if (bound->expressions.size() == 2) {
        expect(std::get<std::int64_t>(bound->expressions[0].value) == 0, "zero is converted");
        expect(std::get<std::int64_t>(bound->expressions[1].value) ==
                   std::numeric_limits<std::int64_t>::max(),
               "maximum signed integer is converted");
        expect(bound->expressions[1].location == SourceLocation{10, 1, 11},
               "source location is preserved");
    }
}

void test_integer_overflow() {
    Parser parser{"SELECT 9223372036854775808;"};
    const auto parsed = parser.parse_select_statement();
    expect(parsed.has_value(), "overflowing literal is syntactically valid");
    if (!parsed) {
        return;
    }

    const auto bound = bind_select_statement(*parsed);
    expect(!bound, "overflowing literal is rejected during binding");
    if (!bound) {
        expect(bound.error().code == BindErrorCode::integer_out_of_range,
               "overflow has expected error code");
        expect(bound.error().location == SourceLocation{7, 1, 8},
               "overflow has expected location");
    }
}

void test_string_literals() {
    Parser parser{"SELECT 'Alice', 'it''s done', '';"};
    const auto parsed = parser.parse_select_statement();
    expect(parsed.has_value(), "string literal is syntactically valid");
    if (!parsed) {
        return;
    }

    const auto bound = bind_select_statement(*parsed);
    expect(bound.has_value(), "string literals are bound");
    if (!bound) {
        return;
    }

    expect(bound->expressions.size() == 3, "three strings are bound");
    if (bound->expressions.size() == 3) {
        expect(std::get<std::string>(bound->expressions[0].value) == "Alice",
               "quotes are removed");
        expect(std::get<std::string>(bound->expressions[1].value) == "it's done",
               "escaped quote is decoded");
        expect(std::get<std::string>(bound->expressions[2].value).empty(),
               "empty string is decoded");
    }
}

void test_null_literal() {
    Parser parser{"SELECT NULL;"};
    const auto parsed = parser.parse_select_statement();
    expect(parsed.has_value(), "NULL literal is syntactically valid");
    if (!parsed) {
        return;
    }

    const auto bound = bind_select_statement(*parsed);
    expect(bound.has_value(), "NULL literal is bound");
    if (!bound) {
        return;
    }

    expect(bound->expressions.size() == 1, "one NULL expression is bound");
    if (bound->expressions.size() == 1) {
        expect(std::holds_alternative<NullValue>(bound->expressions[0].value),
               "bound value contains NULL");
        expect(bound->expressions[0].location == SourceLocation{7, 1, 8},
               "NULL source location is preserved");
    }
}

}  // namespace

int main() {
    test_integer_literals();
    test_integer_overflow();
    test_string_literals();
    test_null_literal();

    if (failures != 0) {
        std::cerr << failures << " binder assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
