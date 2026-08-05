#pragma once

#include "catalog/catalog.hpp"
#include "engine/query.hpp"
#include "sql/ast.hpp"
#include "storage/access/in_memory_heap.hpp"
#include "storage/storage_manager.hpp"

#include <expected>
#include <filesystem>
#include <string_view>
#include <variant>

namespace minidb::engine {

using DatabaseOpenError =
    std::variant<storage::StorageManagerError, catalog::CatalogError>;

class Database {
public:
    Database() = default;

    [[nodiscard]] static std::expected<Database, DatabaseOpenError> create(
        const std::filesystem::path& path);
    [[nodiscard]] static std::expected<Database, DatabaseOpenError> open(
        const std::filesystem::path& path);

    [[nodiscard]] std::expected<QueryResult, QueryError> execute(std::string_view source);

private:
    Database(catalog::Catalog catalog, storage::StorageManager storage);

    [[nodiscard]] std::expected<QueryResult, QueryError> execute(
        const sql::SelectStatement& statement);
    [[nodiscard]] std::expected<QueryResult, QueryError> execute(
        const sql::CreateTableStatement& statement);
    [[nodiscard]] std::expected<QueryResult, QueryError> execute(
        const sql::InsertStatement& statement);
    [[nodiscard]] std::expected<QueryResult, QueryError> execute(
        const sql::DeleteStatement& statement);
    [[nodiscard]] std::expected<QueryResult, QueryError> execute(
        const sql::UpdateStatement& statement);

    catalog::Catalog catalog_;
    std::variant<storage::InMemoryHeap, storage::StorageManager> storage_;
};

}  // namespace minidb::engine
