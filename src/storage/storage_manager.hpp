#pragma once

#include "common/table_id.hpp"
#include "index/btree.hpp"
#include "storage/access/record_id.hpp"
#include "storage/access/row.hpp"
#include "storage/access/stored_row.hpp"
#include "storage/catalog/catalog_store.hpp"
#include "storage/io/database_file.hpp"
#include "types/value.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <variant>
#include <vector>

namespace minidb::storage {

inline constexpr std::size_t default_buffer_pool_capacity = 64;

enum class StorageManagerErrorCode {
    invalid_buffer_pool_capacity,
    table_not_found,
    primary_key_not_found,
    primary_key_null,
    unique_constraint_violation,
    invalid_row_shape,
};

using StorageManagerError =
    std::variant<DatabaseFileError, BufferPoolError, CatalogStoreError,
                 TableHeapError, index::BTreeError,
                 StorageManagerErrorCode>;

class StorageManager {
public:
    [[nodiscard]] static std::expected<StorageManager, StorageManagerError> create(
        const std::filesystem::path& path,
        std::size_t buffer_pool_capacity = default_buffer_pool_capacity);
    [[nodiscard]] static std::expected<StorageManager, StorageManagerError> open(
        const std::filesystem::path& path,
        std::size_t buffer_pool_capacity = default_buffer_pool_capacity);

    StorageManager(const StorageManager&) = delete;
    StorageManager& operator=(const StorageManager&) = delete;
    StorageManager(StorageManager&&) noexcept;
    StorageManager& operator=(StorageManager&&) noexcept;
    ~StorageManager();

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] std::size_t buffer_pool_capacity() const noexcept;
    [[nodiscard]] std::span<const StoredTableMetadata> tables() const noexcept;
    [[nodiscard]] std::expected<StoredTableMetadata, StorageManagerError>
    create_table(catalog::TableSchema schema);
    [[nodiscard]] std::expected<RecordId, StorageManagerError> insert(
        common::TableId table_id, const Row& row);
    [[nodiscard]] std::expected<void, StorageManagerError> update(
        common::TableId table_id, RecordId record_id, const Row& row);
    [[nodiscard]] std::expected<void, StorageManagerError> erase(
        common::TableId table_id, RecordId record_id);
    [[nodiscard]] std::expected<void, StorageManagerError> restore(
        common::TableId table_id, RecordId record_id, const Row& row);
    [[nodiscard]] std::expected<std::vector<Row>, StorageManagerError> scan(
        common::TableId table_id) const;
    [[nodiscard]] std::expected<std::vector<StoredRow>, StorageManagerError>
    scan_records(common::TableId table_id) const;
    [[nodiscard]] std::expected<std::vector<Row>, StorageManagerError>
    lookup_primary_key(common::TableId table_id,
                       const types::Value& value) const;

private:
    struct State;

    explicit StorageManager(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;
};

}  // namespace minidb::storage
