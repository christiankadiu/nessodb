#pragma once

#include "sql/ast.hpp"
#include "sql/lexer.hpp"

#include <expected>
#include <optional>
#include <string_view>

namespace minidb::sql {

enum class ParseErrorCode {
    lexical_error,
    expected_statement,
    expected_explain,
    expected_select,
    expected_literal,
    expected_expression,
    expected_from,
    expected_predicate_operator,
    expected_null,
    expected_comma_from_or_end,
    expected_comma_or_end,
    expected_end_of_input,
    expected_create,
    expected_table,
    expected_identifier,
    expected_left_parenthesis,
    expected_right_parenthesis,
    expected_column_type,
    expected_comma_or_right_parenthesis,
    expected_insert,
    expected_delete,
    expected_update,
    expected_set,
    expected_equal,
    expected_comma_where_or_end,
    expected_into,
    expected_values,
    expected_by,
    expected_integer_literal,
};

struct ParseError {
    ParseErrorCode code;
    SourceLocation location;
};

class Parser {
public:
    explicit Parser(std::string_view source) noexcept;

    [[nodiscard]] std::expected<Statement, ParseError> parse_statement();
    [[nodiscard]] std::expected<ExplainStatement, ParseError> parse_explain_statement();
    [[nodiscard]] std::expected<SelectStatement, ParseError> parse_select_statement();
    [[nodiscard]] std::expected<CreateTableStatement, ParseError> parse_create_table_statement();
    [[nodiscard]] std::expected<InsertStatement, ParseError> parse_insert_statement();
    [[nodiscard]] std::expected<DeleteStatement, ParseError> parse_delete_statement();
    [[nodiscard]] std::expected<UpdateStatement, ParseError> parse_update_statement();

private:
    [[nodiscard]] std::expected<Token, ParseError> next_token();
    [[nodiscard]] std::expected<Token, ParseError> peek_token();
    [[nodiscard]] std::expected<Predicate, ParseError> parse_predicate();
    [[nodiscard]] std::expected<Predicate, ParseError> parse_conjunction();
    [[nodiscard]] std::expected<Predicate, ParseError> parse_primary_predicate();
    [[nodiscard]] std::expected<LimitClause, ParseError> parse_limit_clause();
    [[nodiscard]] std::expected<OffsetClause, ParseError> parse_offset_clause();
    [[nodiscard]] std::expected<void, ParseError> parse_table_statement_tail(
        std::optional<Predicate>& where,
        std::vector<OrderByTerm>* order_by = nullptr,
        std::optional<LimitClause>* limit = nullptr,
        std::optional<OffsetClause>* offset = nullptr);

    Lexer lexer_;
    std::optional<Token> lookahead_;
};

}  // namespace minidb::sql
