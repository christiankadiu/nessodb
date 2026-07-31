#pragma once

#include "common/page_id.hpp"
#include "storage/access/table_heap.hpp"
#include "storage/buffer/buffer_pool.hpp"
#include "storage/catalog/catalog_record.hpp"
#include "storage/io/database_file.hpp"

#include <expected>
#include <variant>

namespace minidb::storage {

enum class CatalogStoreErrorCode {
    already_initialized,
    not_initialized,
    duplicate_table_id,
    duplicate_table_name,
    duplicate_column_name,
};

using CatalogStoreError =
    std::variant<DatabaseFileError, BufferPoolError, TableHeapError,
                 CatalogRecordError, CatalogStoreErrorCode>;

class CatalogStore {
public:
    [[nodiscard]] static std::expected<CatalogStore, CatalogStoreError> create(
        DatabaseFile& database_file, BufferPool& buffer_pool);
    [[nodiscard]] static std::expected<CatalogStore, CatalogStoreError> open(
        DatabaseFile& database_file, BufferPool& buffer_pool);

    CatalogStore(const CatalogStore&) = delete;
    CatalogStore& operator=(const CatalogStore&) = delete;
    CatalogStore(CatalogStore&&) noexcept = default;
    CatalogStore& operator=(CatalogStore&&) = delete;
    ~CatalogStore() = default;

    [[nodiscard]] common::PageId first_page_id() const noexcept;
    [[nodiscard]] std::expected<void, CatalogStoreError> add_table(
        const catalog::TableSchema& schema, common::PageId first_page_id);

private:
    CatalogStore(BufferPool& buffer_pool, TableHeap table_heap);

    BufferPool& buffer_pool_;
    TableHeap table_heap_;
};

}  // namespace minidb::storage
