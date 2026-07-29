#pragma once

#include "sql/ast.hpp"
#include "sql/lexer.hpp"

#include <expected>
#include <string_view>

namespace minidb::sql {

enum class ParseErrorCode {
    lexical_error,
    expected_select,
    expected_integer_literal,
    expected_comma_or_end,
    expected_end_of_input,
};

struct ParseError {
    ParseErrorCode code;
    SourceLocation location;
};

class Parser {
public:
    explicit Parser(std::string_view source) noexcept;

    [[nodiscard]] std::expected<SelectStatement, ParseError> parse_select_statement();

private:
    [[nodiscard]] std::expected<Token, ParseError> next_token();

    Lexer lexer_;
};

}  // namespace minidb::sql
