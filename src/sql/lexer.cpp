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

bool is_select(std::string_view value) noexcept {
    constexpr std::string_view keyword = "select";
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
    if (character == ';') {
        return make_token(TokenType::semicolon, start);
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
    if (is_select(token.lexeme)) {
        token.type = TokenType::select;
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
