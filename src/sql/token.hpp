#pragma once

#include <cstddef>
#include <string_view>

namespace minidb::sql {

enum class TokenType {
    end_of_input,
    identifier,
    integer_literal,
    string_literal,
    null_literal,
    explain,
    select,
    distinct,
    from,
    where,
    is,
    not_keyword,
    and_keyword,
    or_keyword,
    order,
    by,
    asc,
    as_keyword,
    desc,
    limit,
    offset,
    create,
    table,
    insert,
    delete_keyword,
    update,
    set,
    into,
    values,
    integer_type,
    text_type,
    plus,
    minus,
    star,
    slash,
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
    comma,
    semicolon,
    left_parenthesis,
    right_parenthesis,
};

struct SourceLocation {
    std::size_t offset{};
    std::size_t line{1};
    std::size_t column{1};

    friend bool operator==(const SourceLocation&, const SourceLocation&) = default;
};

struct Token {
    TokenType type{TokenType::end_of_input};
    std::string_view lexeme;
    SourceLocation location;
};

}  // namespace minidb::sql
