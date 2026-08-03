#pragma once

#include "sql/ast.hpp"
#include "sql/lexer.hpp"

#include <expected>
#include <string_view>

namespace minidb::sql {

enum class ParseErrorCode {
    lexical_error,
    expected_statement,
    expected_select,
    expected_literal,
    expected_expression,
    expected_from,
    expected_predicate_operator,
    expected_null,
    expected_comma_from_or_end,
    expected_end_of_input,
    expected_create,
    expected_table,
    expected_identifier,
    expected_left_parenthesis,
    expected_column_type,
    expected_comma_or_right_parenthesis,
    expected_insert,
    expected_into,
    expected_values,
};

struct ParseError {
    ParseErrorCode code;
    SourceLocation location;
};

class Parser {
public:
    explicit Parser(std::string_view source) noexcept;

    [[nodiscard]] std::expected<Statement, ParseError> parse_statement();
    [[nodiscard]] std::expected<SelectStatement, ParseError> parse_select_statement();
    [[nodiscard]] std::expected<CreateTableStatement, ParseError> parse_create_table_statement();
    [[nodiscard]] std::expected<InsertStatement, ParseError> parse_insert_statement();

private:
    [[nodiscard]] std::expected<Token, ParseError> next_token();
    [[nodiscard]] std::expected<void, ParseError> parse_select_table_tail(
        SelectStatement& statement);

    Lexer lexer_;
};

}  // namespace minidb::sql
