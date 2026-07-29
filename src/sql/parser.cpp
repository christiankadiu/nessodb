#include "sql/parser.hpp"

namespace minidb::sql {

Parser::Parser(std::string_view source) noexcept : lexer_(source) {}

std::expected<SelectStatement, ParseError> Parser::parse_select_statement() {
    auto token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::select) {
        return std::unexpected(ParseError{ParseErrorCode::expected_select, token->location});
    }

    SelectStatement statement;
    token = next_token();

    while (true) {
        if (!token) {
            return std::unexpected(token.error());
        }
        if (token->type != TokenType::integer_literal && token->type != TokenType::string_literal &&
            token->type != TokenType::null_literal) {
            return std::unexpected(ParseError{ParseErrorCode::expected_literal, token->location});
        }

        LiteralType literal_type = LiteralType::null;
        if (token->type == TokenType::integer_literal) {
            literal_type = LiteralType::integer;
        } else if (token->type == TokenType::string_literal) {
            literal_type = LiteralType::string;
        }
        statement.expressions.push_back(
            LiteralExpression{literal_type, token->lexeme, token->location});

        auto delimiter = next_token();
        if (!delimiter) {
            return std::unexpected(delimiter.error());
        }

        if (delimiter->type == TokenType::comma) {
            token = next_token();
            continue;
        }
        if (delimiter->type == TokenType::end_of_input) {
            return statement;
        }
        if (delimiter->type != TokenType::semicolon) {
            return std::unexpected(
                ParseError{ParseErrorCode::expected_comma_or_end, delimiter->location});
        }

        auto end = next_token();
        if (!end) {
            return std::unexpected(end.error());
        }
        if (end->type != TokenType::end_of_input) {
            return std::unexpected(
                ParseError{ParseErrorCode::expected_end_of_input, end->location});
        }
        return statement;
    }
}

std::expected<Token, ParseError> Parser::next_token() {
    auto token = lexer_.next();
    if (!token) {
        return std::unexpected(
            ParseError{ParseErrorCode::lexical_error, token.error().location});
    }
    return *token;
}

}  // namespace minidb::sql
