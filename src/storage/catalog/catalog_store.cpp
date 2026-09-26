#include "storage/catalog/catalog_store.hpp"

#include "common/identifier.hpp"
#include "storage/catalog/catalog_record.hpp"
#include "storage/page/page.hpp"

#include <algorithm>
#include <cstddef>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nessodb::storage {
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

bool valid_table_root(const DatabaseFile& database_file,
                      common::PageId page_id) noexcept {
    return page_id.is_valid() && page_id.value != 0 &&
           page_id.value < database_file.header().page_count &&
           page_id != database_file.header().catalog_root;
}

bool valid_index_root(const DatabaseFile& database_file,
                      common::PageId page_id) noexcept {
    return page_id.is_valid() && page_id.value != 0 &&
           page_id.value < database_file.header().page_count &&
           page_id != database_file.header().catalog_root;
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
    return CatalogStore{database_file, buffer_pool, std::move(*table_heap)};
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
    return CatalogStore{database_file, buffer_pool, std::move(*table_heap)};
}

common::PageId CatalogStore::first_page_id() const noexcept {
    return table_heap_.first_page_id();
}

std::expected<void, CatalogStoreError> CatalogStore::validate_table(
    const catalog::TableSchema& schema) const {
    auto table_row = encode_catalog_record(CatalogTableRecord{
        schema.id, schema.name, common::PageId{1}, schema.columns.size()});
    if (!table_row) {
        return std::unexpected(CatalogStoreError{table_row.error()});
    }
    auto valid_table_row = validate_catalog_row(*table_row);
    if (!valid_table_row) {
        return std::unexpected(valid_table_row.error());
    }

    bool has_primary_key = false;
    for (std::size_t ordinal = 0; ordinal < schema.columns.size(); ++ordinal) {
        for (std::size_t existing = 0; existing < ordinal; ++existing) {
            if (common::identifiers_equal(schema.columns[ordinal].name,
                                          schema.columns[existing].name)) {
                return std::unexpected(CatalogStoreError{
                    CatalogStoreErrorCode::duplicate_column_name});
            }
        }
        if (schema.columns[ordinal].primary_key && has_primary_key) {
            return std::unexpected(CatalogStoreError{
                CatalogStoreErrorCode::multiple_primary_keys});
        }
        has_primary_key =
            has_primary_key || schema.columns[ordinal].primary_key;
        auto column_row = encode_catalog_record(CatalogColumnRecord{
            schema.id, static_cast<std::uint64_t>(ordinal), schema.columns[ordinal]});
        if (!column_row) {
            return std::unexpected(CatalogStoreError{column_row.error()});
        }
        auto valid_column_row = validate_catalog_row(*column_row);
        if (!valid_column_row) {
            return std::unexpected(valid_column_row.error());
        }
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
    return {};
}

std::expected<void, CatalogStoreError> CatalogStore::add_table(
    const catalog::TableSchema& schema, common::PageId first_page_id,
    std::vector<StoredIndexMetadata> indexes) {
    if (!valid_table_root(database_file_, first_page_id)) {
        return std::unexpected(
            CatalogStoreError{CatalogStoreErrorCode::invalid_table_root});
    }
    auto valid_table = validate_table(schema);
    if (!valid_table) {
        return std::unexpected(valid_table.error());
    }

    std::size_t primary_index_count = 0;
    for (const auto& index : indexes) {
        if (index.column_index >= schema.columns.size()) {
            return std::unexpected(CatalogStoreError{
                CatalogStoreErrorCode::invalid_index_column});
        }
        if (!valid_index_root(database_file_, index.root_page_id) ||
            index.root_page_id == first_page_id) {
            return std::unexpected(CatalogStoreError{
                CatalogStoreErrorCode::invalid_index_root});
        }
        if (index.primary_key &&
            (!index.unique ||
             !schema.columns[index.column_index].primary_key)) {
            return std::unexpected(CatalogStoreError{
                CatalogStoreErrorCode::invalid_index_column});
        }
        primary_index_count += index.primary_key ? 1 : 0;
    }
    const bool schema_has_primary_key = std::any_of(
        schema.columns.begin(), schema.columns.end(),
        [](const catalog::ColumnSchema& column) {
            return column.primary_key;
        });
    if (primary_index_count != (schema_has_primary_key ? 1U : 0U)) {
        return std::unexpected(CatalogStoreError{
            CatalogStoreErrorCode::missing_primary_index});
    }

    auto table_row = encode_catalog_record(
        CatalogTableRecord{schema.id, schema.name, first_page_id,
                           schema.columns.size()});
    if (!table_row) {
        return std::unexpected(CatalogStoreError{table_row.error()});
    }

    std::vector<Row> column_rows;
    column_rows.reserve(schema.columns.size());
    for (std::size_t ordinal = 0; ordinal < schema.columns.size(); ++ordinal) {
        auto column_row = encode_catalog_record(CatalogColumnRecord{
            schema.id, static_cast<std::uint64_t>(ordinal), schema.columns[ordinal]});
        if (!column_row) {
            return std::unexpected(CatalogStoreError{column_row.error()});
        }
        column_rows.push_back(std::move(*column_row));
    }

    std::vector<Row> index_rows;
    index_rows.reserve(indexes.size());
    for (const auto& index : indexes) {
        auto index_row = encode_catalog_record(CatalogIndexRecord{
            schema.id, static_cast<std::uint64_t>(index.column_index),
            index.name, index.root_page_id, index.unique,
            index.primary_key});
        if (!index_row) {
            return std::unexpected(CatalogStoreError{index_row.error()});
        }
        index_rows.push_back(std::move(*index_row));
    }

    for (const auto& row : column_rows) {
        auto inserted = table_heap_.insert(row);
        if (!inserted) {
            return std::unexpected(CatalogStoreError{inserted.error()});
        }
    }
    for (const auto& row : index_rows) {
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

std::expected<void, CatalogStoreError> CatalogStore::update_index_root(
    common::TableId table_id, const StoredIndexMetadata& index) {
    if (!table_id.is_valid() ||
        !valid_index_root(database_file_, index.root_page_id)) {
        return std::unexpected(CatalogStoreError{
            CatalogStoreErrorCode::invalid_index_root});
    }
    auto row = encode_catalog_record(CatalogIndexRecord{
        table_id, static_cast<std::uint64_t>(index.column_index),
        index.name, index.root_page_id, index.unique,
        index.primary_key});
    if (!row) {
        return std::unexpected(CatalogStoreError{row.error()});
    }
    auto inserted = table_heap_.insert(*row);
    if (!inserted) {
        return std::unexpected(CatalogStoreError{inserted.error()});
    }
    auto flushed = buffer_pool_.flush();
    if (!flushed) {
        return std::unexpected(CatalogStoreError{flushed.error()});
    }
    return {};
}

std::expected<std::vector<StoredTableMetadata>, CatalogStoreError>
CatalogStore::load_tables() const {
    auto rows = table_heap_.scan();
    if (!rows) {
        return std::unexpected(CatalogStoreError{rows.error()});
    }

    std::unordered_map<std::uint64_t, std::vector<CatalogColumnRecord>>
        pending_columns;
    std::unordered_map<std::uint64_t, std::vector<CatalogIndexRecord>>
        pending_indexes;
    std::vector<CatalogTableRecord> table_records;
    for (const auto& row : *rows) {
        auto record = decode_catalog_record(row);
        if (!record) {
            return std::unexpected(CatalogStoreError{record.error()});
        }
        if (auto* column = std::get_if<CatalogColumnRecord>(&*record)) {
            pending_columns[column->table_id.value].push_back(std::move(*column));
            continue;
        }
        if (auto* index = std::get_if<CatalogIndexRecord>(&*record)) {
            pending_indexes[index->table_id.value].push_back(
                std::move(*index));
            continue;
        }
        table_records.push_back(std::get<CatalogTableRecord>(std::move(*record)));
    }

    std::vector<StoredTableMetadata> tables;
    tables.reserve(table_records.size());
    for (const auto& table : table_records) {
        if (!valid_table_root(database_file_, table.first_page_id)) {
            return std::unexpected(
                CatalogStoreError{CatalogStoreErrorCode::invalid_table_root});
        }
        for (const auto& stored : tables) {
            if (stored.schema.id == table.table_id) {
                return std::unexpected(
                    CatalogStoreError{CatalogStoreErrorCode::duplicate_table_id});
            }
            if (common::identifiers_equal(stored.schema.name, table.name)) {
                return std::unexpected(
                    CatalogStoreError{CatalogStoreErrorCode::duplicate_table_name});
            }
        }

        auto& candidates = pending_columns[table.table_id.value];
        if (table.column_count > candidates.size()) {
            return std::unexpected(
                CatalogStoreError{CatalogStoreErrorCode::incomplete_table});
        }
        const auto first_column =
            candidates.size() - static_cast<std::size_t>(table.column_count);

        catalog::TableSchema schema;
        schema.name = table.name;
        schema.id = table.table_id;
        schema.columns.reserve(static_cast<std::size_t>(table.column_count));
        for (std::size_t ordinal = 0;
             ordinal < static_cast<std::size_t>(table.column_count); ++ordinal) {
            auto& column = candidates[first_column + ordinal];
            if (column.ordinal != ordinal) {
                return std::unexpected(
                    CatalogStoreError{CatalogStoreErrorCode::invalid_column_order});
            }
            for (const auto& existing : schema.columns) {
                if (common::identifiers_equal(existing.name, column.column.name)) {
                    return std::unexpected(CatalogStoreError{
                        CatalogStoreErrorCode::duplicate_column_name});
                }
            }
            schema.columns.push_back(std::move(column.column));
        }
        pending_columns.erase(table.table_id.value);

        std::vector<StoredIndexMetadata> indexes;
        auto& stored_indexes = pending_indexes[table.table_id.value];
        for (const auto& index : stored_indexes) {
            if (index.ordinal >= schema.columns.size() ||
                !valid_index_root(database_file_, index.root_page_id) ||
                index.root_page_id == table.first_page_id) {
                return std::unexpected(CatalogStoreError{
                    CatalogStoreErrorCode::invalid_index_root});
            }
            const auto column_index =
                static_cast<std::size_t>(index.ordinal);
            if (index.primary_key &&
                (!index.unique ||
                 !schema.columns[column_index].primary_key)) {
                return std::unexpected(CatalogStoreError{
                    CatalogStoreErrorCode::invalid_index_column});
            }
            const auto existing = std::find_if(
                indexes.begin(), indexes.end(),
                [&index](const StoredIndexMetadata& candidate) {
                    return candidate.column_index == index.ordinal &&
                           candidate.name == index.name;
                });
            StoredIndexMetadata metadata{
                index.name, column_index, index.root_page_id,
                index.unique, index.primary_key};
            if (existing == indexes.end()) {
                indexes.push_back(std::move(metadata));
            } else {
                *existing = std::move(metadata);
            }
        }
        pending_indexes.erase(table.table_id.value);

        const auto primary_columns = static_cast<std::size_t>(std::count_if(
            schema.columns.begin(), schema.columns.end(),
            [](const catalog::ColumnSchema& column) {
                return column.primary_key;
            }));
        const auto primary_indexes = static_cast<std::size_t>(std::count_if(
            indexes.begin(), indexes.end(),
            [](const StoredIndexMetadata& index) {
                return index.primary_key;
            }));
        if (primary_columns > 1 || primary_indexes != primary_columns) {
            return std::unexpected(CatalogStoreError{
                primary_columns > 1
                    ? CatalogStoreErrorCode::multiple_primary_keys
                    : CatalogStoreErrorCode::missing_primary_index});
        }
        tables.push_back(StoredTableMetadata{
            std::move(schema), table.first_page_id, std::move(indexes)});
    }
    return tables;
}

CatalogStore::CatalogStore(DatabaseFile& database_file, BufferPool& buffer_pool,
                           TableHeap table_heap)
    : database_file_(database_file),
      buffer_pool_(buffer_pool),
      table_heap_(std::move(table_heap)) {}

}  // namespace nessodb::storage
