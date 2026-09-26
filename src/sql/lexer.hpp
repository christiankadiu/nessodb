#pragma once

#include "sql/token.hpp"

#include <expected>
#include <string_view>

namespace nessodb::sql {

enum class LexErrorCode {
    invalid_character,
    unterminated_string,
};

struct LexError {
    LexErrorCode code;
    char character{};
    SourceLocation location;
};

class Lexer {
public:
    explicit Lexer(std::string_view source) noexcept;

    [[nodiscard]] std::expected<Token, LexError> next();

private:
    [[nodiscard]] bool at_end() const noexcept;
    [[nodiscard]] char peek() const noexcept;
    char advance() noexcept;
    void skip_whitespace() noexcept;

    [[nodiscard]] Token scan_identifier(SourceLocation start) noexcept;
    [[nodiscard]] Token scan_integer(SourceLocation start) noexcept;
    [[nodiscard]] std::expected<Token, LexError> scan_string(SourceLocation start) noexcept;
    [[nodiscard]] Token make_token(TokenType type, SourceLocation start) const noexcept;

    std::string_view source_;
    std::size_t offset_{};
    std::size_t line_{1};
    std::size_t column_{1};
};

}  // namespace nessodb::sql
