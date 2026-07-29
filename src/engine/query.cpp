#include "engine/query.hpp"

namespace minidb::engine {

std::expected<QueryResult, QueryError> execute_query(std::string_view source) {
    sql::Parser parser{source};
    auto parsed = parser.parse_select_statement();
    if (!parsed) {
        return std::unexpected(QueryError{parsed.error()});
    }

    auto bound = binder::bind_select_statement(*parsed);
    if (!bound) {
        return std::unexpected(QueryError{bound.error()});
    }

    QueryResult result;
    result.values.reserve(bound->expressions.size());
    for (const auto& expression : bound->expressions) {
        result.values.push_back(expression.value);
    }
    return result;
}

}  // namespace minidb::engine
