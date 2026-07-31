#include "storage/catalog/catalog_store.hpp"

#include "common/identifier.hpp"
#include "storage/catalog/catalog_record.hpp"
#include "storage/page/page.hpp"

#include <cstddef>
#include <utility>
#include <vector>

namespace minidb::storage {
namespace {

std::expected<void, CatalogStoreError> validate_catalog_row(const Row& row) {
    auto encoded = encode_record(row, catalog_record_schema());
    if (!encoded) {
        return std::unexpected(
            CatalogStoreError{TableHeapError{encoded.error()}});
    }
    constexpr std::size_t maximum_record_size =
        page_size - heap_page_header_size - slot_entry_size;
    if (encoded->size() > maximum_record_size) {
        return std::unexpected(CatalogStoreError{
            TableHeapError{TableHeapErrorCode::record_too_large}});
    }
    return {};
}

}  // namespace

std::expected<CatalogStore, CatalogStoreError> CatalogStore::create(
    DatabaseFile& database_file, BufferPool& buffer_pool) {
    if (database_file.header().catalog_root) {
        return std::unexpected(
            CatalogStoreError{CatalogStoreErrorCode::already_initialized});
    }

    auto table_heap = TableHeap::create(buffer_pool, catalog_record_schema());
    if (!table_heap) {
        return std::unexpected(CatalogStoreError{table_heap.error()});
    }
    auto initialized =
        database_file.initialize_catalog_root(table_heap->first_page_id());
    if (!initialized) {
        return std::unexpected(CatalogStoreError{initialized.error()});
    }
    return CatalogStore{buffer_pool, std::move(*table_heap)};
}

std::expected<CatalogStore, CatalogStoreError> CatalogStore::open(
    DatabaseFile& database_file, BufferPool& buffer_pool) {
    if (!database_file.header().catalog_root) {
        return std::unexpected(
            CatalogStoreError{CatalogStoreErrorCode::not_initialized});
    }

    auto table_heap = TableHeap::open(
        buffer_pool, catalog_record_schema(),
        *database_file.header().catalog_root);
    if (!table_heap) {
        return std::unexpected(CatalogStoreError{table_heap.error()});
    }
    return CatalogStore{buffer_pool, std::move(*table_heap)};
}

common::PageId CatalogStore::first_page_id() const noexcept {
    return table_heap_.first_page_id();
}

std::expected<void, CatalogStoreError> CatalogStore::add_table(
    const catalog::TableSchema& schema, common::PageId first_page_id) {
    auto table_row = encode_catalog_record(
        CatalogTableRecord{schema.id, schema.name, first_page_id,
                           schema.columns.size()});
    if (!table_row) {
        return std::unexpected(CatalogStoreError{table_row.error()});
    }
    auto valid_table_row = validate_catalog_row(*table_row);
    if (!valid_table_row) {
        return std::unexpected(valid_table_row.error());
    }

    std::vector<Row> column_rows;
    column_rows.reserve(schema.columns.size());
    for (std::size_t ordinal = 0; ordinal < schema.columns.size(); ++ordinal) {
        for (std::size_t existing = 0; existing < ordinal; ++existing) {
            if (common::identifiers_equal(schema.columns[ordinal].name,
                                          schema.columns[existing].name)) {
                return std::unexpected(CatalogStoreError{
                    CatalogStoreErrorCode::duplicate_column_name});
            }
        }

        auto column_row = encode_catalog_record(CatalogColumnRecord{
            schema.id, static_cast<std::uint64_t>(ordinal), schema.columns[ordinal]});
        if (!column_row) {
            return std::unexpected(CatalogStoreError{column_row.error()});
        }
        auto valid_column_row = validate_catalog_row(*column_row);
        if (!valid_column_row) {
            return std::unexpected(valid_column_row.error());
        }
        column_rows.push_back(std::move(*column_row));
    }

    auto existing_rows = table_heap_.scan();
    if (!existing_rows) {
        return std::unexpected(CatalogStoreError{existing_rows.error()});
    }
    for (const auto& row : *existing_rows) {
        auto record = decode_catalog_record(row);
        if (!record) {
            return std::unexpected(CatalogStoreError{record.error()});
        }
        const auto* table = std::get_if<CatalogTableRecord>(&*record);
        if (table == nullptr) {
            continue;
        }
        if (table->table_id == schema.id) {
            return std::unexpected(
                CatalogStoreError{CatalogStoreErrorCode::duplicate_table_id});
        }
        if (common::identifiers_equal(table->name, schema.name)) {
            return std::unexpected(
                CatalogStoreError{CatalogStoreErrorCode::duplicate_table_name});
        }
    }

    for (const auto& row : column_rows) {
        auto inserted = table_heap_.insert(row);
        if (!inserted) {
            return std::unexpected(CatalogStoreError{inserted.error()});
        }
    }
    auto columns_flushed = buffer_pool_.flush();
    if (!columns_flushed) {
        return std::unexpected(CatalogStoreError{columns_flushed.error()});
    }

    auto table_inserted = table_heap_.insert(*table_row);
    if (!table_inserted) {
        return std::unexpected(CatalogStoreError{table_inserted.error()});
    }
    auto table_flushed = buffer_pool_.flush();
    if (!table_flushed) {
        return std::unexpected(CatalogStoreError{table_flushed.error()});
    }
    return {};
}

CatalogStore::CatalogStore(BufferPool& buffer_pool, TableHeap table_heap)
    : buffer_pool_(buffer_pool), table_heap_(std::move(table_heap)) {}

}  // namespace minidb::storage
