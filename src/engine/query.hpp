#pragma once

#include "binder/binder.hpp"
#include "sql/parser.hpp"

#include <cstdint>
#include <expected>
#include <string_view>
#include <variant>
#include <vector>

namespace minidb::engine {

struct IntegerQueryResult {
    std::vector<std::int64_t> values;
};

using QueryError = std::variant<sql::ParseError, binder::BindError>;

[[nodiscard]] std::expected<IntegerQueryResult, QueryError> execute_query(
    std::string_view source);

}  // namespace minidb::engine
