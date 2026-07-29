#pragma once

#include "catalog/catalog.hpp"
#include "engine/query.hpp"
#include "sql/ast.hpp"

#include <expected>
#include <string_view>

namespace minidb::engine {

class Database {
public:
    [[nodiscard]] std::expected<QueryResult, QueryError> execute(std::string_view source);

private:
    [[nodiscard]] std::expected<QueryResult, QueryError> execute(
        const sql::SelectStatement& statement);
    [[nodiscard]] std::expected<QueryResult, QueryError> execute(
        const sql::CreateTableStatement& statement);

    catalog::Catalog catalog_;
};

}  // namespace minidb::engine
