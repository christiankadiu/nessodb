#include "engine/database.hpp"

#include "binder/binder.hpp"
#include "execution/expression_evaluator.hpp"
#include "execution/select_executor.hpp"
#include "planner/logical_planner.hpp"
#include "planner/physical_plan_formatter.hpp"
#include "planner/physical_planner.hpp"
#include "sql/parser.hpp"

#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace minidb::engine {
namespace {

void apply_assignments(
    storage::Row& row,
    const std::vector<binder::BoundUpdateAssignment>& assignments) {
    for (const auto& assignment : assignments) {
        if (assignment.column_index >= row.values.size()) {
            throw std::logic_error{
                "table schema and stored row state diverged"};
        }
        row.values[assignment.column_index] = assignment.value;
    }
}

bool matches_where_clause(std::span<const types::Value> values,
                          const binder::BoundExpression& expression) {
    if (expression.result_type() !=
        binder::BoundExpressionResultType::boolean) {
        throw std::logic_error{"WHERE expression must be boolean"};
    }
    const auto result = execution::evaluate_expression(values, expression);
    const auto* matches = std::get_if<bool>(&result);
    if (matches == nullptr) {
        throw std::logic_error{
            "WHERE expression does not produce a boolean"};
    }
    return *matches;
}

}  // namespace

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

    std::vector<storage::Row> stored_rows;
    if (bound->table_id.is_valid()) {
        if (const auto* heap = std::get_if<storage::InMemoryHeap>(&storage_)) {
            auto scanned = heap->scan(bound->table_id);
            if (!scanned) {
                throw std::logic_error{"catalog and heap table state diverged"};
            }
            stored_rows = std::move(*scanned);
        } else {
            auto scanned = std::get<storage::StorageManager>(storage_).scan(
                bound->table_id);
            if (!scanned) {
                return std::unexpected(QueryError{StorageError{
                    std::move(scanned.error())}});
            }
            stored_rows = std::move(*scanned);
        }
    }

    auto selected_rows =
        execution::execute_select(std::move(*bound), std::move(stored_rows));
    if (!selected_rows) {
        return std::unexpected(QueryError{ExecutionError{
            ExecutionErrorCode::memory_limit_exceeded, {}}});
    }
    QueryResult result;
    result.rows.reserve(selected_rows->size());
    for (auto& row : *selected_rows) {
        result.rows.push_back(ResultRow{std::move(row.values)});
    }
    return result;
}

std::expected<QueryResult, QueryError> Database::execute(
    const sql::ExplainStatement& statement) {
    auto bound = binder::bind_select_statement(statement.select, catalog_);
    if (!bound) {
        return std::unexpected(QueryError{bound.error()});
    }

    auto logical_plan = planner::plan_select(std::move(*bound));
    auto physical_plan = planner::plan_physical(std::move(logical_plan));
    auto lines = planner::format_physical_plan(*physical_plan);

    QueryResult result;
    result.rows.reserve(lines.size());
    for (auto& line : lines) {
        ResultRow row;
        row.values.emplace_back(std::move(line));
        result.rows.push_back(std::move(row));
    }
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

std::expected<QueryResult, QueryError> Database::execute(
    const sql::DeleteStatement& statement) {
    auto bound = binder::bind_delete_statement(statement, catalog_);
    if (!bound) {
        return std::unexpected(QueryError{bound.error()});
    }

    QueryResult result;
    if (auto* heap = std::get_if<storage::InMemoryHeap>(&storage_)) {
        auto records = heap->scan_records(bound->table_id);
        if (!records) {
            throw std::logic_error{"catalog and heap table state diverged"};
        }

        for (const auto& record : *records) {
            if (bound->where &&
                !matches_where_clause(record.row.values, *bound->where)) {
                continue;
            }
            auto erased = heap->erase(bound->table_id, record.row_id);
            if (!erased) {
                throw std::logic_error{"heap row state diverged during deletion"};
            }
            ++result.rows_affected;
        }
        return result;
    }

    auto& storage = std::get<storage::StorageManager>(storage_);
    auto records = storage.scan_records(bound->table_id);
    if (!records) {
        return std::unexpected(
            QueryError{StorageError{std::move(records.error())}});
    }
    for (const auto& record : *records) {
        if (bound->where &&
            !matches_where_clause(record.row.values, *bound->where)) {
            continue;
        }
        auto erased = storage.erase(bound->table_id, record.record_id);
        if (!erased) {
            return std::unexpected(
                QueryError{StorageError{std::move(erased.error())}});
        }
        ++result.rows_affected;
    }
    return result;
}

std::expected<QueryResult, QueryError> Database::execute(
    const sql::UpdateStatement& statement) {
    auto bound = binder::bind_update_statement(statement, catalog_);
    if (!bound) {
        return std::unexpected(QueryError{bound.error()});
    }

    QueryResult result;
    if (auto* heap = std::get_if<storage::InMemoryHeap>(&storage_)) {
        auto records = heap->scan_records(bound->table_id);
        if (!records) {
            throw std::logic_error{"catalog and heap table state diverged"};
        }

        for (auto& record : *records) {
            if (bound->where &&
                !matches_where_clause(record.row.values, *bound->where)) {
                continue;
            }
            apply_assignments(record.row, bound->assignments);
            auto updated = heap->update(bound->table_id, record.row_id,
                                        std::move(record.row));
            if (!updated) {
                throw std::logic_error{
                    "heap row state diverged during update"};
            }
            ++result.rows_affected;
        }
        return result;
    }

    auto& storage = std::get<storage::StorageManager>(storage_);
    auto records = storage.scan_records(bound->table_id);
    if (!records) {
        return std::unexpected(
            QueryError{StorageError{std::move(records.error())}});
    }
    for (auto& record : *records) {
        if (bound->where &&
            !matches_where_clause(record.row.values, *bound->where)) {
            continue;
        }
        apply_assignments(record.row, bound->assignments);
        auto updated = storage.update(bound->table_id, record.record_id,
                                      record.row);
        if (!updated) {
            return std::unexpected(
                QueryError{StorageError{std::move(updated.error())}});
        }
        ++result.rows_affected;
    }
    return result;
}

}  // namespace minidb::engine
