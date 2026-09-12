#include "sql/lexer.hpp"

#include <cctype>

namespace minidb::sql {
namespace {

bool is_identifier_start(char character) noexcept {
    return std::isalpha(static_cast<unsigned char>(character)) != 0 || character == '_';
}

bool is_identifier_part(char character) noexcept {
    return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
}

bool equals_case_insensitive(std::string_view value, std::string_view keyword) noexcept {
    if (value.size() != keyword.size()) {
        return false;
    }

    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto character = static_cast<unsigned char>(value[index]);
        if (std::tolower(character) != keyword[index]) {
            return false;
        }
    }
    return true;
}

}  // namespace

Lexer::Lexer(std::string_view source) noexcept : source_(source) {}

std::expected<Token, LexError> Lexer::next() {
    skip_whitespace();

    const SourceLocation start{offset_, line_, column_};
    if (at_end()) {
        return Token{TokenType::end_of_input, {}, start};
    }

    if (is_identifier_start(peek())) {
        return scan_identifier(start);
    }
    if (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
        return scan_integer(start);
    }
    if (peek() == '\'') {
        return scan_string(start);
    }

    const char character = advance();
    if (character == ',') {
        return make_token(TokenType::comma, start);
    }
    if (character == '.') {
        return make_token(TokenType::dot, start);
    }
    if (character == '+') {
        return make_token(TokenType::plus, start);
    }
    if (character == '-') {
        return make_token(TokenType::minus, start);
    }
    if (character == '*') {
        return make_token(TokenType::star, start);
    }
    if (character == '/') {
        return make_token(TokenType::slash, start);
    }
    if (character == '=') {
        return make_token(TokenType::equal, start);
    }
    if (character == '!' && !at_end() && peek() == '=') {
        advance();
        return make_token(TokenType::not_equal, start);
    }
    if (character == '<') {
        if (!at_end() && peek() == '=') {
            advance();
            return make_token(TokenType::less_equal, start);
        }
        return make_token(TokenType::less, start);
    }
    if (character == '>') {
        if (!at_end() && peek() == '=') {
            advance();
            return make_token(TokenType::greater_equal, start);
        }
        return make_token(TokenType::greater, start);
    }
    if (character == ';') {
        return make_token(TokenType::semicolon, start);
    }
    if (character == '(') {
        return make_token(TokenType::left_parenthesis, start);
    }
    if (character == ')') {
        return make_token(TokenType::right_parenthesis, start);
    }

    return std::unexpected(LexError{LexErrorCode::invalid_character, character, start});
}

bool Lexer::at_end() const noexcept {
    return offset_ >= source_.size();
}

char Lexer::peek() const noexcept {
    return source_[offset_];
}

char Lexer::advance() noexcept {
    const char character = source_[offset_++];
    if (character == '\r') {
        if (!at_end() && source_[offset_] == '\n') {
            ++offset_;
        }
        ++line_;
        column_ = 1;
    } else if (character == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return character;
}

void Lexer::skip_whitespace() noexcept {
    while (!at_end() && std::isspace(static_cast<unsigned char>(peek())) != 0) {
        advance();
    }
}

Token Lexer::scan_identifier(SourceLocation start) noexcept {
    while (!at_end() && is_identifier_part(peek())) {
        advance();
    }

    Token token = make_token(TokenType::identifier, start);
    if (equals_case_insensitive(token.lexeme, "explain")) {
        token.type = TokenType::explain;
    } else if (equals_case_insensitive(token.lexeme, "select")) {
        token.type = TokenType::select;
    } else if (equals_case_insensitive(token.lexeme, "distinct")) {
        token.type = TokenType::distinct;
    } else if (equals_case_insensitive(token.lexeme, "from")) {
        token.type = TokenType::from;
    } else if (equals_case_insensitive(token.lexeme, "join")) {
        token.type = TokenType::join;
    } else if (equals_case_insensitive(token.lexeme, "on")) {
        token.type = TokenType::on;
    } else if (equals_case_insensitive(token.lexeme, "where")) {
        token.type = TokenType::where;
    } else if (equals_case_insensitive(token.lexeme, "group")) {
        token.type = TokenType::group;
    } else if (equals_case_insensitive(token.lexeme, "is")) {
        token.type = TokenType::is;
    } else if (equals_case_insensitive(token.lexeme, "not")) {
        token.type = TokenType::not_keyword;
    } else if (equals_case_insensitive(token.lexeme, "and")) {
        token.type = TokenType::and_keyword;
    } else if (equals_case_insensitive(token.lexeme, "or")) {
        token.type = TokenType::or_keyword;
    } else if (equals_case_insensitive(token.lexeme, "order")) {
        token.type = TokenType::order;
    } else if (equals_case_insensitive(token.lexeme, "by")) {
        token.type = TokenType::by;
    } else if (equals_case_insensitive(token.lexeme, "asc")) {
        token.type = TokenType::asc;
    } else if (equals_case_insensitive(token.lexeme, "as")) {
        token.type = TokenType::as_keyword;
    } else if (equals_case_insensitive(token.lexeme, "desc")) {
        token.type = TokenType::desc;
    } else if (equals_case_insensitive(token.lexeme, "limit")) {
        token.type = TokenType::limit;
    } else if (equals_case_insensitive(token.lexeme, "offset")) {
        token.type = TokenType::offset;
    } else if (equals_case_insensitive(token.lexeme, "null")) {
        token.type = TokenType::null_literal;
    } else if (equals_case_insensitive(token.lexeme, "create")) {
        token.type = TokenType::create;
    } else if (equals_case_insensitive(token.lexeme, "table")) {
        token.type = TokenType::table;
    } else if (equals_case_insensitive(token.lexeme, "primary")) {
        token.type = TokenType::primary;
    } else if (equals_case_insensitive(token.lexeme, "key")) {
        token.type = TokenType::key;
    } else if (equals_case_insensitive(token.lexeme, "insert")) {
        token.type = TokenType::insert;
    } else if (equals_case_insensitive(token.lexeme, "delete")) {
        token.type = TokenType::delete_keyword;
    } else if (equals_case_insensitive(token.lexeme, "update")) {
        token.type = TokenType::update;
    } else if (equals_case_insensitive(token.lexeme, "begin")) {
        token.type = TokenType::begin;
    } else if (equals_case_insensitive(token.lexeme, "commit")) {
        token.type = TokenType::commit;
    } else if (equals_case_insensitive(token.lexeme, "rollback")) {
        token.type = TokenType::rollback;
    } else if (equals_case_insensitive(token.lexeme, "set")) {
        token.type = TokenType::set;
    } else if (equals_case_insensitive(token.lexeme, "into")) {
        token.type = TokenType::into;
    } else if (equals_case_insensitive(token.lexeme, "values")) {
        token.type = TokenType::values;
    } else if (equals_case_insensitive(token.lexeme, "int")) {
        token.type = TokenType::integer_type;
    } else if (equals_case_insensitive(token.lexeme, "text")) {
        token.type = TokenType::text_type;
    }
    return token;
}

Token Lexer::scan_integer(SourceLocation start) noexcept {
    while (!at_end() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
        advance();
    }
    return make_token(TokenType::integer_literal, start);
}

std::expected<Token, LexError> Lexer::scan_string(SourceLocation start) noexcept {
    advance();

    while (!at_end()) {
        if (peek() != '\'') {
            advance();
            continue;
        }

        advance();
        if (!at_end() && peek() == '\'') {
            advance();
            continue;
        }

        return make_token(TokenType::string_literal, start);
    }

    return std::unexpected(LexError{LexErrorCode::unterminated_string, '\'', start});
}

Token Lexer::make_token(TokenType type, SourceLocation start) const noexcept {
    return Token{type, source_.substr(start.offset, offset_ - start.offset), start};
}

}  // namespace minidb::sql
