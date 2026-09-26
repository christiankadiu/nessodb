#pragma once

#include "catalog/catalog.hpp"
#include "engine/query.hpp"
#include "recovery/wal.hpp"
#include "sql/ast.hpp"
#include "storage/access/in_memory_heap.hpp"
#include "storage/storage_manager.hpp"
#include "storage/undo_log.hpp"
#include "transaction/transaction_manager.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <variant>

namespace minidb::engine {

using DatabaseOpenError =
    std::variant<storage::StorageManagerError, catalog::CatalogError,
                 recovery::WalError>;

class Database {
public:
    Database() = default;

    [[nodiscard]] static std::expected<Database, DatabaseOpenError> create(
        const std::filesystem::path& path);
    [[nodiscard]] static std::expected<Database, DatabaseOpenError> open(
        const std::filesystem::path& path);

    [[nodiscard]] std::expected<QueryResult, QueryError> execute(std::string_view source);
    [[nodiscard]] std::expected<void, QueryError> begin_transaction();
    [[nodiscard]] std::expected<void, QueryError> commit_transaction();
    [[nodiscard]] std::expected<void, QueryError> rollback_transaction();
    [[nodiscard]] bool has_active_transaction() const noexcept;

private:
    struct ActiveTransaction {
        transaction::TransactionHandle handle;
        storage::UndoLog undo;
        bool explicit_transaction{};
    };

    Database(catalog::Catalog catalog, storage::StorageManager storage,
             recovery::WriteAheadLog wal);

    [[nodiscard]] std::expected<void, QueryError> start_transaction(
        bool explicit_transaction);
    [[nodiscard]] std::expected<void, QueryError> rollback_to(
        std::size_t undo_position);
    [[nodiscard]] std::expected<void, QueryError> acquire_table_lock(
        ActiveTransaction& transaction, common::TableId table_id,
        transaction::LockMode mode);

    [[nodiscard]] std::expected<QueryResult, QueryError> execute(
        const sql::SelectStatement& statement, ActiveTransaction& transaction);
    [[nodiscard]] std::expected<QueryResult, QueryError> execute(
        const sql::ExplainStatement& statement, ActiveTransaction& transaction);
    [[nodiscard]] std::expected<QueryResult, QueryError> execute(
        const sql::CreateTableStatement& statement, ActiveTransaction& transaction);
    [[nodiscard]] std::expected<QueryResult, QueryError> execute(
        const sql::InsertStatement& statement, ActiveTransaction& transaction);
    [[nodiscard]] std::expected<QueryResult, QueryError> execute(
        const sql::DeleteStatement& statement, ActiveTransaction& transaction);
    [[nodiscard]] std::expected<QueryResult, QueryError> execute(
        const sql::UpdateStatement& statement, ActiveTransaction& transaction);

    catalog::Catalog catalog_;
    std::optional<recovery::WriteAheadLog> wal_;
    std::variant<storage::InMemoryHeap, storage::StorageManager> storage_;
    std::unique_ptr<transaction::TransactionManager> transaction_manager_{
        std::make_unique<transaction::TransactionManager>()};
    std::optional<ActiveTransaction> active_transaction_;
};

}  // namespace minidb::engine
