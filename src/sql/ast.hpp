#pragma once

#include "sql/token.hpp"

#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace minidb::sql {

enum class LiteralType {
    integer,
    string,
    null,
};

struct LiteralExpression {
    LiteralType type;
    std::string_view text;
    SourceLocation location;
};

struct TableReference {
    std::string_view name;
    SourceLocation location;
};

struct SelectStatement {
    std::vector<LiteralExpression> expressions;
    bool select_all_columns{};
    std::optional<TableReference> from;
};

enum class ColumnType {
    integer,
    text,
};

struct ColumnDefinition {
    std::string_view name;
    ColumnType type;
    SourceLocation location;
};

struct CreateTableStatement {
    std::string_view table_name;
    SourceLocation table_location;
    std::vector<ColumnDefinition> columns;
};

struct InsertStatement {
    std::string_view table_name;
    SourceLocation table_location;
    std::vector<LiteralExpression> values;
};

using Statement = std::variant<SelectStatement, CreateTableStatement, InsertStatement>;

}  // namespace minidb::sql
