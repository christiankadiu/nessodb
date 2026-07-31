#pragma once

#include "common/page_id.hpp"
#include "storage/access/table_heap.hpp"
#include "storage/buffer/buffer_pool.hpp"
#include "storage/io/database_file.hpp"

#include <expected>
#include <variant>

namespace minidb::storage {

enum class CatalogStoreErrorCode {
    already_initialized,
    not_initialized,
};

using CatalogStoreError =
    std::variant<DatabaseFileError, TableHeapError, CatalogStoreErrorCode>;

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

private:
    explicit CatalogStore(TableHeap table_heap);

    TableHeap table_heap_;
};

}  // namespace minidb::storage
