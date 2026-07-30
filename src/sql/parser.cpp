#include "sql/parser.hpp"

#include <utility>

namespace minidb::sql {
namespace {

bool is_literal(TokenType type) noexcept {
    return type == TokenType::integer_literal || type == TokenType::string_literal ||
           type == TokenType::null_literal;
}

LiteralType literal_type(TokenType type) noexcept {
    if (type == TokenType::integer_literal) {
        return LiteralType::integer;
    }
    if (type == TokenType::string_literal) {
        return LiteralType::string;
    }
    return LiteralType::null;
}

}  // namespace

Parser::Parser(std::string_view source) noexcept : lexer_(source) {}

std::expected<Statement, ParseError> Parser::parse_statement() {
    Lexer statement_lexer = lexer_;
    auto token = statement_lexer.next();
    if (!token) {
        return std::unexpected(
            ParseError{ParseErrorCode::lexical_error, token.error().location});
    }

    if (token->type == TokenType::select) {
        auto statement = parse_select_statement();
        if (!statement) {
            return std::unexpected(statement.error());
        }
        return Statement{std::move(*statement)};
    }
    if (token->type == TokenType::create) {
        auto statement = parse_create_table_statement();
        if (!statement) {
            return std::unexpected(statement.error());
        }
        return Statement{std::move(*statement)};
    }
    if (token->type == TokenType::insert) {
        auto statement = parse_insert_statement();
        if (!statement) {
            return std::unexpected(statement.error());
        }
        return Statement{std::move(*statement)};
    }
    return std::unexpected(ParseError{ParseErrorCode::expected_statement, token->location});
}

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
        if (!is_literal(token->type)) {
            return std::unexpected(ParseError{ParseErrorCode::expected_literal, token->location});
        }

        statement.expressions.push_back(
            LiteralExpression{literal_type(token->type), token->lexeme, token->location});

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

std::expected<CreateTableStatement, ParseError> Parser::parse_create_table_statement() {
    auto token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::create) {
        return std::unexpected(ParseError{ParseErrorCode::expected_create, token->location});
    }

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::table) {
        return std::unexpected(ParseError{ParseErrorCode::expected_table, token->location});
    }

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::identifier) {
        return std::unexpected(ParseError{ParseErrorCode::expected_identifier, token->location});
    }

    CreateTableStatement statement{token->lexeme, token->location, {}};

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::left_parenthesis) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_left_parenthesis, token->location});
    }

    token = next_token();
    while (true) {
        if (!token) {
            return std::unexpected(token.error());
        }
        if (token->type != TokenType::identifier) {
            return std::unexpected(
                ParseError{ParseErrorCode::expected_identifier, token->location});
        }

        const Token column_name = *token;
        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }

        ColumnType column_type;
        if (token->type == TokenType::integer_type) {
            column_type = ColumnType::integer;
        } else if (token->type == TokenType::text_type) {
            column_type = ColumnType::text;
        } else {
            return std::unexpected(
                ParseError{ParseErrorCode::expected_column_type, token->location});
        }
        statement.columns.push_back(
            ColumnDefinition{column_name.lexeme, column_type, column_name.location});

        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        if (token->type == TokenType::right_parenthesis) {
            break;
        }
        if (token->type != TokenType::comma) {
            return std::unexpected(ParseError{
                ParseErrorCode::expected_comma_or_right_parenthesis, token->location});
        }
        token = next_token();
    }

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type == TokenType::semicolon) {
        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
    }
    if (token->type != TokenType::end_of_input) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_end_of_input, token->location});
    }

    return statement;
}

std::expected<InsertStatement, ParseError> Parser::parse_insert_statement() {
    auto token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::insert) {
        return std::unexpected(ParseError{ParseErrorCode::expected_insert, token->location});
    }

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::into) {
        return std::unexpected(ParseError{ParseErrorCode::expected_into, token->location});
    }

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::identifier) {
        return std::unexpected(ParseError{ParseErrorCode::expected_identifier, token->location});
    }
    InsertStatement statement{token->lexeme, token->location, {}};

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::values) {
        return std::unexpected(ParseError{ParseErrorCode::expected_values, token->location});
    }

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::left_parenthesis) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_left_parenthesis, token->location});
    }

    token = next_token();
    while (true) {
        if (!token) {
            return std::unexpected(token.error());
        }
        if (!is_literal(token->type)) {
            return std::unexpected(ParseError{ParseErrorCode::expected_literal, token->location});
        }
        statement.values.push_back(
            LiteralExpression{literal_type(token->type), token->lexeme, token->location});

        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        if (token->type == TokenType::right_parenthesis) {
            break;
        }
        if (token->type != TokenType::comma) {
            return std::unexpected(ParseError{
                ParseErrorCode::expected_comma_or_right_parenthesis, token->location});
        }
        token = next_token();
    }

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type == TokenType::semicolon) {
        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
    }
    if (token->type != TokenType::end_of_input) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_end_of_input, token->location});
    }
    return statement;
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
