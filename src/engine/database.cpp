#include "engine/database.hpp"

#include "binder/binder.hpp"
#include "sql/parser.hpp"

#include <stdexcept>
#include <utility>
#include <variant>

namespace minidb::engine {

Database::Database(catalog::Catalog catalog, storage::StorageManager storage)
    : catalog_(std::move(catalog)), storage_(std::move(storage)) {}

std::expected<Database, DatabaseOpenError> Database::create(
    const std::filesystem::path& path) {
    auto storage = storage::StorageManager::create(path);
    if (!storage) {
        return std::unexpected(DatabaseOpenError{std::move(storage.error())});
    }
    return Database{catalog::Catalog{}, std::move(*storage)};
}

std::expected<Database, DatabaseOpenError> Database::open(
    const std::filesystem::path& path) {
    auto storage = storage::StorageManager::open(path);
    if (!storage) {
        return std::unexpected(DatabaseOpenError{std::move(storage.error())});
    }

    catalog::Catalog catalog;
    for (const auto& table : storage->tables()) {
        auto restored = catalog.restore_table(table.schema);
        if (!restored) {
            return std::unexpected(DatabaseOpenError{restored.error()});
        }
    }
    return Database{std::move(catalog), std::move(*storage)};
}

std::expected<QueryResult, QueryError> Database::execute(std::string_view source) {
    sql::Parser parser{source};
    auto parsed = parser.parse_statement();
    if (!parsed) {
        return std::unexpected(QueryError{parsed.error()});
    }

    return std::visit([this](const auto& statement) { return execute(statement); }, *parsed);
}

std::expected<QueryResult, QueryError> Database::execute(
    const sql::SelectStatement& statement) {
    auto bound = binder::bind_select_statement(statement, catalog_);
    if (!bound) {
        return std::unexpected(QueryError{bound.error()});
    }

    if (bound->table_id.is_valid()) {
        std::vector<storage::Row> stored_rows;
        if (const auto* heap = std::get_if<storage::InMemoryHeap>(&storage_)) {
            auto scanned = heap->scan(bound->table_id);
            if (!scanned) {
                throw std::logic_error{"catalog and heap table state diverged"};
            }
            stored_rows.assign(scanned->begin(), scanned->end());
        } else {
            auto scanned = std::get<storage::StorageManager>(storage_).scan(
                bound->table_id);
            if (!scanned) {
                return std::unexpected(QueryError{StorageError{
                    std::move(scanned.error())}});
            }
            stored_rows = std::move(*scanned);
        }

        QueryResult result;
        result.rows.reserve(stored_rows.size());
        for (auto& stored_row : stored_rows) {
            if (bound->where) {
                const auto column_index = bound->where->column_index;
                if (column_index >= stored_row.values.size()) {
                    throw std::logic_error{
                        "table schema and stored row state diverged"};
                }
                const auto& stored_value = stored_row.values[column_index];
                const auto& expected_value = bound->where->value;
                if (std::holds_alternative<types::NullValue>(stored_value) ||
                    std::holds_alternative<types::NullValue>(expected_value) ||
                    stored_value != expected_value) {
                    continue;
                }
            }

            if (bound->expressions.empty()) {
                result.rows.push_back(ResultRow{std::move(stored_row.values)});
                continue;
            }

            ResultRow projected_row;
            projected_row.values.reserve(bound->expressions.size());
            for (const auto& expression : bound->expressions) {
                if (const auto* literal =
                        std::get_if<binder::BoundLiteralExpression>(&expression)) {
                    projected_row.values.push_back(literal->value);
                    continue;
                }

                const auto column_index =
                    std::get<binder::BoundColumnReferenceExpression>(expression)
                        .column_index;
                if (column_index >= stored_row.values.size()) {
                    throw std::logic_error{
                        "table schema and stored row state diverged"};
                }
                projected_row.values.push_back(
                    stored_row.values[column_index]);
            }
            result.rows.push_back(std::move(projected_row));
        }
        return result;
    }

    ResultRow row;
    row.values.reserve(bound->expressions.size());
    for (const auto& expression : bound->expressions) {
        row.values.push_back(
            std::get<binder::BoundLiteralExpression>(expression).value);
    }
    QueryResult result;
    result.rows.push_back(std::move(row));
    return result;
}

std::expected<QueryResult, QueryError> Database::execute(
    const sql::CreateTableStatement& statement) {
    auto bound = binder::bind_create_table_statement(statement);
    if (!bound) {
        return std::unexpected(QueryError{bound.error()});
    }

    catalog::TableSchema schema;
    schema.name = std::move(bound->table_name);
    schema.columns.reserve(bound->columns.size());
    for (auto& column : bound->columns) {
        schema.columns.push_back(catalog::ColumnSchema{std::move(column.name), column.type});
    }

    auto updated_catalog = catalog_;
    auto created = updated_catalog.create_table(std::move(schema));
    if (!created) {
        return std::unexpected(QueryError{ExecutionError{
            ExecutionErrorCode::table_already_exists, statement.table_location}});
    }

    if (auto* heap = std::get_if<storage::InMemoryHeap>(&storage_)) {
        auto heap_created = heap->create_table((*created)->id);
        if (!heap_created) {
            throw std::logic_error{"catalog and heap table state diverged"};
        }
    } else {
        auto stored = std::get<storage::StorageManager>(storage_).create_table(
            **created);
        if (!stored) {
            return std::unexpected(QueryError{StorageError{
                std::move(stored.error())}});
        }
    }
    catalog_ = std::move(updated_catalog);
    return QueryResult{};
}

std::expected<QueryResult, QueryError> Database::execute(
    const sql::InsertStatement& statement) {
    auto bound = binder::bind_insert_statement(statement, catalog_);
    if (!bound) {
        return std::unexpected(QueryError{bound.error()});
    }

    storage::Row row{std::move(bound->values)};
    if (auto* heap = std::get_if<storage::InMemoryHeap>(&storage_)) {
        auto inserted = heap->insert(bound->table_id, std::move(row));
        if (!inserted) {
            throw std::logic_error{"catalog and heap table state diverged"};
        }
    } else {
        auto inserted = std::get<storage::StorageManager>(storage_).insert(
            bound->table_id, row);
        if (!inserted) {
            return std::unexpected(QueryError{StorageError{
                std::move(inserted.error())}});
        }
    }
    return QueryResult{{}, 1};
}

}  // namespace minidb::engine
