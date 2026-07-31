#include "storage/catalog/catalog_store.hpp"

#include "catalog/schema.hpp"
#include "types/logical_type.hpp"

#include <utility>

namespace minidb::storage {
namespace {

catalog::TableSchema catalog_schema() {
    return catalog::TableSchema{
        "minidb_catalog",
        {
            {"format_version", types::LogicalType::integer},
            {"entry_kind", types::LogicalType::integer},
            {"table_id", types::LogicalType::integer},
            {"ordinal", types::LogicalType::integer},
            {"name", types::LogicalType::text},
            {"logical_type", types::LogicalType::integer},
            {"first_page_id", types::LogicalType::integer},
        },
    };
}

}  // namespace

std::expected<CatalogStore, CatalogStoreError> CatalogStore::create(
    DatabaseFile& database_file, BufferPool& buffer_pool) {
    if (database_file.header().catalog_root) {
        return std::unexpected(
            CatalogStoreError{CatalogStoreErrorCode::already_initialized});
    }

    auto table_heap = TableHeap::create(buffer_pool, catalog_schema());
    if (!table_heap) {
        return std::unexpected(CatalogStoreError{table_heap.error()});
    }
    auto initialized =
        database_file.initialize_catalog_root(table_heap->first_page_id());
    if (!initialized) {
        return std::unexpected(CatalogStoreError{initialized.error()});
    }
    return CatalogStore{std::move(*table_heap)};
}

std::expected<CatalogStore, CatalogStoreError> CatalogStore::open(
    DatabaseFile& database_file, BufferPool& buffer_pool) {
    if (!database_file.header().catalog_root) {
        return std::unexpected(
            CatalogStoreError{CatalogStoreErrorCode::not_initialized});
    }

    auto table_heap = TableHeap::open(
        buffer_pool, catalog_schema(), *database_file.header().catalog_root);
    if (!table_heap) {
        return std::unexpected(CatalogStoreError{table_heap.error()});
    }
    return CatalogStore{std::move(*table_heap)};
}

common::PageId CatalogStore::first_page_id() const noexcept {
    return table_heap_.first_page_id();
}

CatalogStore::CatalogStore(TableHeap table_heap)
    : table_heap_(std::move(table_heap)) {}

}  // namespace minidb::storage
