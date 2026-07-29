#pragma once

#include "binder/binder.hpp"
#include "sql/parser.hpp"
#include "types/value.hpp"

#include <expected>
#include <string_view>
#include <variant>
#include <vector>

namespace minidb::engine {

struct QueryResult {
    std::vector<types::Value> values;
};

using QueryError = std::variant<sql::ParseError, binder::BindError>;

[[nodiscard]] std::expected<QueryResult, QueryError> execute_query(std::string_view source);

}  // namespace minidb::engine
