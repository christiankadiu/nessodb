#include "engine/database.hpp"

#include "binder/binder.hpp"
#include "execution/expression_evaluator.hpp"
#include "execution/select_executor.hpp"
#include "planner/logical_planner.hpp"
#include "planner/physical_plan_formatter.hpp"
#include "planner/physical_planner.hpp"
#include "sql/parser.hpp"
#include "storage/io/page_file.hpp"

#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
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
    if (!result) {
        throw std::logic_error{
            "WHERE expression produced an arithmetic error"};
    }
    const auto* matches = std::get_if<bool>(&*result);
    if (matches == nullptr) {
        throw std::logic_error{
            "WHERE expression does not produce a boolean"};
    }
    return *matches;
}

std::optional<storage::StorageManagerErrorCode> primary_key_violation(
    const catalog::TableSchema& schema, const storage::Row& candidate,
    std::span<const storage::InMemoryStoredRow> existing_rows,
    std::optional<storage::InMemoryRowId> excluded_row = std::nullopt) {
    for (std::size_t column_index = 0;
         column_index < schema.columns.size(); ++column_index) {
        if (!schema.columns[column_index].primary_key) {
            continue;
        }
        if (column_index >= candidate.values.size()) {
            return storage::StorageManagerErrorCode::invalid_row_shape;
        }
        const auto& value = candidate.values[column_index];
        if (std::holds_alternative<types::NullValue>(value)) {
            return storage::StorageManagerErrorCode::primary_key_null;
        }
        for (const auto& existing : existing_rows) {
            if ((!excluded_row || existing.row_id != *excluded_row) &&
                column_index < existing.row.values.size() &&
                existing.row.values[column_index] == value) {
                return storage::StorageManagerErrorCode::
                    unique_constraint_violation;
            }
        }
        break;
    }
    return std::nullopt;
}

ExecutionError execution_error(execution::OperatorError error) noexcept {
    switch (error.code) {
        case execution::OperatorErrorCode::memory_limit_exceeded:
            return ExecutionError{
                ExecutionErrorCode::memory_limit_exceeded, error.location};
        case execution::OperatorErrorCode::integer_overflow:
            return ExecutionError{
                ExecutionErrorCode::integer_overflow, error.location};
        case execution::OperatorErrorCode::division_by_zero:
            return ExecutionError{
                ExecutionErrorCode::division_by_zero, error.location};
        case execution::OperatorErrorCode::type_mismatch:
            return ExecutionError{
                ExecutionErrorCode::type_mismatch, error.location};
    }
    return ExecutionError{
        ExecutionErrorCode::memory_limit_exceeded, error.location};
}

std::expected<QueryResult, QueryError> transaction_control_result(
    std::expected<void, QueryError> result) {
    if (!result) {
        return std::unexpected(std::move(result.error()));
    }
    return QueryResult{};
}

std::expected<recovery::WriteAheadLog, DatabaseOpenError>
open_wal_and_recover(const std::filesystem::path& path) {
    auto page_file = storage::PageFile::open(path);
    if (!page_file) {
        return std::unexpected(DatabaseOpenError{
            storage::StorageManagerError{
                storage::DatabaseFileError{page_file.error()}}});
    }
    auto wal = recovery::WriteAheadLog::open(path);
    if (!wal) {
        return std::unexpected(DatabaseOpenError{wal.error()});
    }
    auto recovered = wal->recover(*page_file);
    if (!recovered) {
        return std::unexpected(DatabaseOpenError{recovered.error()});
    }
    return std::move(*wal);
}

}  // namespace

Database::Database(catalog::Catalog catalog, storage::StorageManager storage,
                   recovery::WriteAheadLog wal)
    : catalog_(std::move(catalog)), wal_(std::move(wal)),
      storage_(std::move(storage)) {}

std::expected<Database, DatabaseOpenError> Database::create(
    const std::filesystem::path& path) {
    auto storage = storage::StorageManager::create(path);
    if (!storage) {
        return std::unexpected(DatabaseOpenError{std::move(storage.error())});
    }
    auto wal = recovery::WriteAheadLog::create(path);
    if (!wal) {
        return std::unexpected(DatabaseOpenError{wal.error()});
    }
    return Database{
        catalog::Catalog{}, std::move(*storage), std::move(*wal)};
}

std::expected<Database, DatabaseOpenError> Database::open(
    const std::filesystem::path& path) {
    auto wal = open_wal_and_recover(path);
    if (!wal) {
        return std::unexpected(DatabaseOpenError{wal.error()});
    }

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
    return Database{
        std::move(catalog), std::move(*storage), std::move(*wal)};
}

std::expected<QueryResult, QueryError> Database::execute(std::string_view source) {
    sql::Parser parser{source};
    auto parsed = parser.parse_statement();
    if (!parsed) {
        return std::unexpected(QueryError{parsed.error()});
    }

    if (std::holds_alternative<sql::BeginStatement>(*parsed)) {
        return transaction_control_result(begin_transaction());
    }
    if (std::holds_alternative<sql::CommitStatement>(*parsed)) {
        return transaction_control_result(commit_transaction());
    }
    if (std::holds_alternative<sql::RollbackStatement>(*parsed)) {
        return transaction_control_result(rollback_transaction());
    }

    const bool autocommit = !active_transaction_;
    if (autocommit) {
        auto started = start_transaction(false);
        if (!started) {
            return std::unexpected(started.error());
        }
    }

    const std::size_t undo_position = active_transaction_->undo.size();
    std::expected<QueryResult, QueryError> result;
    try {
        result = std::visit(
            [this](const auto& statement)
                -> std::expected<QueryResult, QueryError> {
                using Statement = std::remove_cvref_t<decltype(statement)>;
                if constexpr (std::is_same_v<Statement, sql::BeginStatement> ||
                              std::is_same_v<Statement, sql::CommitStatement> ||
                              std::is_same_v<Statement,
                                             sql::RollbackStatement>) {
                    throw std::logic_error{
                        "transaction control statement reached query execution"};
                } else {
                    return execute(statement, *active_transaction_);
                }
            },
            *parsed);
    } catch (...) {
        const auto undone = rollback_to(undo_position);
        if (autocommit && undone) {
            (void)rollback_transaction();
        }
        throw;
    }

    if (!result) {
        auto undone = rollback_to(undo_position);
        if (!undone) {
            return std::unexpected(undone.error());
        }
        if (autocommit) {
            auto rolled_back = rollback_transaction();
            if (!rolled_back) {
                return std::unexpected(rolled_back.error());
            }
        }
        return std::unexpected(result.error());
    }

    if (autocommit) {
        auto committed = commit_transaction();
        if (!committed) {
            return std::unexpected(committed.error());
        }
    }
    return result;
}

std::expected<void, QueryError> Database::begin_transaction() {
    return start_transaction(true);
}

std::expected<void, QueryError> Database::commit_transaction() {
    if (!active_transaction_) {
        return std::unexpected(QueryError{TransactionExecutionError{
            TransactionExecutionErrorCode::no_active_transaction}});
    }

    if (wal_) {
        auto logged = wal_->commit(active_transaction_->handle->id());
        if (!logged) {
            return std::unexpected(QueryError{RecoveryError{logged.error()}});
        }
    }
    auto committed = transaction_manager_->commit(active_transaction_->handle);
    if (!committed) {
        return std::unexpected(QueryError{TransactionExecutionError{
            TransactionExecutionErrorCode::transaction_state_error}});
    }
    active_transaction_.reset();
    return {};
}

std::expected<void, QueryError> Database::rollback_transaction() {
    if (!active_transaction_) {
        return std::unexpected(QueryError{TransactionExecutionError{
            TransactionExecutionErrorCode::no_active_transaction}});
    }

    auto undone = rollback_to(0);
    if (!undone) {
        return std::unexpected(undone.error());
    }
    if (wal_) {
        auto logged = wal_->abort(active_transaction_->handle->id());
        if (!logged) {
            return std::unexpected(QueryError{RecoveryError{logged.error()}});
        }
    }
    auto rolled_back =
        transaction_manager_->rollback(active_transaction_->handle);
    if (!rolled_back) {
        return std::unexpected(QueryError{TransactionExecutionError{
            TransactionExecutionErrorCode::transaction_state_error}});
    }
    active_transaction_.reset();
    return {};
}

bool Database::has_active_transaction() const noexcept {
    return active_transaction_.has_value();
}

std::expected<void, QueryError> Database::start_transaction(
    bool explicit_transaction) {
    if (active_transaction_) {
        return std::unexpected(QueryError{TransactionExecutionError{
            TransactionExecutionErrorCode::transaction_already_active}});
    }
    auto started = transaction_manager_->begin();
    if (!started) {
        return std::unexpected(QueryError{TransactionExecutionError{
            TransactionExecutionErrorCode::transaction_state_error}});
    }
    if (wal_) {
        auto* persistent = std::get_if<storage::StorageManager>(&storage_);
        if (persistent == nullptr) {
            throw std::logic_error{
                "persistent WAL requires persistent storage"};
        }
        auto logged = wal_->begin((*started)->id(), persistent->page_count());
        if (!logged) {
            (void)transaction_manager_->rollback(*started);
            return std::unexpected(QueryError{RecoveryError{logged.error()}});
        }
    }
    active_transaction_.emplace(ActiveTransaction{
        std::move(*started), storage::UndoLog{}, explicit_transaction});
    return {};
}

std::expected<void, QueryError> Database::rollback_to(
    std::size_t undo_position) {
    if (!active_transaction_) {
        return std::unexpected(QueryError{TransactionExecutionError{
            TransactionExecutionErrorCode::no_active_transaction}});
    }
    auto undone = std::visit(
        [this, undo_position](auto& storage) {
            return active_transaction_->undo.rollback_to(
                undo_position, storage);
        },
        storage_);
    if (!undone) {
        return std::unexpected(QueryError{TransactionExecutionError{
            TransactionExecutionErrorCode::rollback_failed}});
    }
    return {};
}

std::expected<void, QueryError> Database::acquire_table_lock(
    ActiveTransaction& transaction, common::TableId table_id,
    transaction::LockMode mode) {
    auto acquired = transaction_manager_->acquire_table_lock(
        transaction.handle, table_id, mode);
    if (!acquired) {
        const bool conflict = std::holds_alternative<transaction::LockError>(
            acquired.error()) &&
            std::get<transaction::LockError>(acquired.error()).code ==
                transaction::LockErrorCode::lock_conflict;
        return std::unexpected(QueryError{TransactionExecutionError{
            conflict ? TransactionExecutionErrorCode::lock_conflict
                     : TransactionExecutionErrorCode::transaction_state_error}});
    }
    return {};
}

std::expected<QueryResult, QueryError> Database::execute(
    const sql::SelectStatement& statement, ActiveTransaction& transaction) {
    auto bound = binder::bind_select_statement(statement, catalog_);
    if (!bound) {
        return std::unexpected(QueryError{bound.error()});
    }
    auto result_column_names =
        std::move(bound->result_column_names);
    if (std::holds_alternative<storage::InMemoryHeap>(storage_)) {
        bound->primary_key_lookup.reset();
    }

    std::vector<common::TableId> table_ids;
    if (bound->table_id.is_valid()) {
        table_ids.push_back(bound->table_id);
    }
    for (const auto& join : bound->joins) {
        table_ids.push_back(join.table_id);
    }
    for (const auto table_id : table_ids) {
        auto locked = acquire_table_lock(
            transaction, table_id, transaction::LockMode::shared);
        if (!locked) {
            return std::unexpected(locked.error());
        }
    }

    std::vector<execution::TableInput> table_inputs;
    table_inputs.reserve(table_ids.size());
    for (const auto table_id : table_ids) {
        std::vector<storage::Row> stored_rows;
        if (const auto* heap = std::get_if<storage::InMemoryHeap>(&storage_)) {
            auto scanned = heap->scan(table_id);
            if (!scanned) {
                throw std::logic_error{"catalog and heap table state diverged"};
            }
            stored_rows = std::move(*scanned);
        } else {
            auto& storage_manager =
                std::get<storage::StorageManager>(storage_);
            auto scanned = bound->primary_key_lookup &&
                                   table_id == bound->table_id
                               ? storage_manager.lookup_primary_key(
                                     table_id,
                                     bound->primary_key_lookup->value)
                               : storage_manager.scan(table_id);
            if (!scanned) {
                return std::unexpected(QueryError{StorageError{
                    std::move(scanned.error())}});
            }
            stored_rows = std::move(*scanned);
        }
        table_inputs.push_back(execution::TableInput{
            table_id, std::move(stored_rows)});
    }

    auto selected_rows = execution::execute_select_from_tables(
        std::move(*bound), std::move(table_inputs));
    if (!selected_rows) {
        return std::unexpected(QueryError{
            execution_error(selected_rows.error())});
    }
    QueryResult result;
    result.column_names = std::move(result_column_names);
    result.rows.reserve(selected_rows->size());
    for (auto& row : *selected_rows) {
        result.rows.push_back(ResultRow{std::move(row.values)});
    }
    return result;
}

std::expected<QueryResult, QueryError> Database::execute(
    const sql::ExplainStatement& statement, ActiveTransaction& transaction) {
    auto bound = binder::bind_select_statement(statement.select, catalog_);
    if (!bound) {
        return std::unexpected(QueryError{bound.error()});
    }
    if (std::holds_alternative<storage::InMemoryHeap>(storage_)) {
        bound->primary_key_lookup.reset();
    }
    if (bound->table_id.is_valid()) {
        auto locked = acquire_table_lock(
            transaction, bound->table_id, transaction::LockMode::shared);
        if (!locked) {
            return std::unexpected(locked.error());
        }
    }
    for (const auto& join : bound->joins) {
        auto locked = acquire_table_lock(
            transaction, join.table_id, transaction::LockMode::shared);
        if (!locked) {
            return std::unexpected(locked.error());
        }
    }

    auto logical_plan = planner::plan_select(std::move(*bound));
    auto physical_plan = planner::plan_physical(std::move(logical_plan));
    auto lines = planner::format_physical_plan(*physical_plan);

    QueryResult result;
    result.column_names.emplace_back("QUERY PLAN");
    result.rows.reserve(lines.size());
    for (auto& line : lines) {
        ResultRow row;
        row.values.emplace_back(std::move(line));
        result.rows.push_back(std::move(row));
    }
    return result;
}

std::expected<QueryResult, QueryError> Database::execute(
    const sql::CreateTableStatement& statement, ActiveTransaction& transaction) {
    if (transaction.explicit_transaction) {
        return std::unexpected(QueryError{TransactionExecutionError{
            TransactionExecutionErrorCode::ddl_not_supported}});
    }
    auto bound = binder::bind_create_table_statement(statement);
    if (!bound) {
        return std::unexpected(QueryError{bound.error()});
    }

    catalog::TableSchema schema;
    schema.name = std::move(bound->table_name);
    schema.columns.reserve(bound->columns.size());
    for (auto& column : bound->columns) {
        schema.columns.push_back(catalog::ColumnSchema{
            std::move(column.name), column.type, column.primary_key});
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
    const sql::InsertStatement& statement, ActiveTransaction& transaction) {
    auto bound = binder::bind_insert_statement(statement, catalog_);
    if (!bound) {
        return std::unexpected(QueryError{bound.error()});
    }
    auto locked = acquire_table_lock(
        transaction, bound->table_id, transaction::LockMode::exclusive);
    if (!locked) {
        return std::unexpected(locked.error());
    }

    storage::Row row{std::move(bound->values)};
    if (auto* heap = std::get_if<storage::InMemoryHeap>(&storage_)) {
        const auto* schema = catalog_.find_table(bound->table_id);
        auto records = heap->scan_records(bound->table_id);
        if (schema == nullptr || !records) {
            throw std::logic_error{"catalog and heap table state diverged"};
        }
        if (const auto violation = primary_key_violation(
                *schema, row, *records)) {
            return std::unexpected(QueryError{StorageError{
                storage::StorageManagerError{*violation}}});
        }
        auto inserted = heap->insert(bound->table_id, std::move(row));
        if (!inserted) {
            throw std::logic_error{"catalog and heap table state diverged"};
        }
        transaction.undo.record_insert(bound->table_id, *inserted);
    } else {
        auto inserted = std::get<storage::StorageManager>(storage_).insert(
            bound->table_id, row);
        if (!inserted) {
            return std::unexpected(QueryError{StorageError{
                std::move(inserted.error())}});
        }
        transaction.undo.record_insert(bound->table_id, *inserted);
    }
    QueryResult result;
    result.rows_affected = 1;
    return result;
}

std::expected<QueryResult, QueryError> Database::execute(
    const sql::DeleteStatement& statement, ActiveTransaction& transaction) {
    auto bound = binder::bind_delete_statement(statement, catalog_);
    if (!bound) {
        return std::unexpected(QueryError{bound.error()});
    }
    auto locked = acquire_table_lock(
        transaction, bound->table_id, transaction::LockMode::exclusive);
    if (!locked) {
        return std::unexpected(locked.error());
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
            transaction.undo.record_delete(
                bound->table_id, record.row_id, record.row);
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
        transaction.undo.record_delete(
            bound->table_id, record.record_id, record.row);
        ++result.rows_affected;
    }
    return result;
}

std::expected<QueryResult, QueryError> Database::execute(
    const sql::UpdateStatement& statement, ActiveTransaction& transaction) {
    auto bound = binder::bind_update_statement(statement, catalog_);
    if (!bound) {
        return std::unexpected(QueryError{bound.error()});
    }
    auto locked = acquire_table_lock(
        transaction, bound->table_id, transaction::LockMode::exclusive);
    if (!locked) {
        return std::unexpected(locked.error());
    }

    QueryResult result;
    if (auto* heap = std::get_if<storage::InMemoryHeap>(&storage_)) {
        auto records = heap->scan_records(bound->table_id);
        if (!records) {
            throw std::logic_error{"catalog and heap table state diverged"};
        }
        const auto* schema = catalog_.find_table(bound->table_id);
        if (schema == nullptr) {
            throw std::logic_error{"catalog and heap table state diverged"};
        }

        for (auto& record : *records) {
            if (bound->where &&
                !matches_where_clause(record.row.values, *bound->where)) {
                continue;
            }
            storage::Row before = record.row;
            apply_assignments(record.row, bound->assignments);
            auto current_records = heap->scan_records(bound->table_id);
            if (!current_records) {
                throw std::logic_error{
                    "heap table state diverged during update"};
            }
            if (const auto violation = primary_key_violation(
                    *schema, record.row, *current_records,
                    record.row_id)) {
                return std::unexpected(QueryError{StorageError{
                    storage::StorageManagerError{*violation}}});
            }
            auto updated = heap->update(bound->table_id, record.row_id,
                                        std::move(record.row));
            if (!updated) {
                throw std::logic_error{
                    "heap row state diverged during update"};
            }
            transaction.undo.record_update(
                bound->table_id, record.row_id, std::move(before));
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
        storage::Row before = record.row;
        apply_assignments(record.row, bound->assignments);
        auto updated = storage.update(bound->table_id, record.record_id,
                                      record.row);
        if (!updated) {
            return std::unexpected(
                QueryError{StorageError{std::move(updated.error())}});
        }
        transaction.undo.record_update(
            bound->table_id, record.record_id, std::move(before));
        ++result.rows_affected;
    }
    return result;
}

}  // namespace minidb::engine
