#include "engine/query.hpp"

#include "engine/database.hpp"

namespace minidb::engine {

std::expected<QueryResult, QueryError> execute_query(std::string_view source) {
    Database database;
    return database.execute(source);
}

}  // namespace minidb::engine
