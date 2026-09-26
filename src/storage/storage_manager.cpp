#include "storage/storage_manager.hpp"

#include "index/key_codec.hpp"
#include "storage/buffer/buffer_pool.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace minidb::storage {

namespace {

std::optional<std::size_t> primary_key_column(
    const catalog::TableSchema& schema) {
    for (std::size_t index = 0; index < schema.columns.size(); ++index) {
        if (schema.columns[index].primary_key) {
            return index;
        }
    }
    return std::nullopt;
}

const StoredIndexMetadata* primary_index(
    const StoredTableMetadata& table) {
    const auto found = std::find_if(
        table.indexes.begin(), table.indexes.end(),
        [](const StoredIndexMetadata& index) {
            return index.primary_key;
        });
    return found == table.indexes.end() ? nullptr : &*found;
}

StoredIndexMetadata* primary_index(StoredTableMetadata& table) {
    return const_cast<StoredIndexMetadata*>(
        primary_index(std::as_const(table)));
}

const StoredTableMetadata* find_table_metadata(
    const std::vector<StoredTableMetadata>& tables,
    common::TableId table_id) {
    const auto found = std::find_if(
        tables.begin(), tables.end(),
        [table_id](const StoredTableMetadata& table) {
            return table.schema.id == table_id;
        });
    return found == tables.end() ? nullptr : &*found;
}

StoredTableMetadata* find_table_metadata(
    std::vector<StoredTableMetadata>& tables,
    common::TableId table_id) {
    return const_cast<StoredTableMetadata*>(
        find_table_metadata(std::as_const(tables), table_id));
}

std::expected<index::EncodedKey, StorageManagerError>
encode_primary_key(const Row& row, std::size_t column_index) {
    if (column_index >= row.values.size()) {
        return std::unexpected(StorageManagerError{
            StorageManagerErrorCode::invalid_row_shape});
    }
    if (std::holds_alternative<types::NullValue>(
            row.values[column_index])) {
        return std::unexpected(StorageManagerError{
            StorageManagerErrorCode::primary_key_null});
    }
    return index::encode_key(std::span<const types::Value>{
        &row.values[column_index], 1});
}

std::expected<void, StorageManagerError> persist_primary_root(
    common::TableId table_id,
    std::vector<StoredTableMetadata>& tables,
    std::unordered_map<std::uint64_t, index::BTree>& indexes,
    CatalogStore& catalog_store) {
    const auto table = std::find_if(
        tables.begin(), tables.end(),
        [table_id](const StoredTableMetadata& candidate) {
            return candidate.schema.id == table_id;
        });
    const auto tree = indexes.find(table_id.value);
    if (table == tables.end() || tree == indexes.end()) {
        return std::unexpected(StorageManagerError{
            StorageManagerErrorCode::primary_key_not_found});
    }
    auto* metadata = primary_index(*table);
    if (metadata == nullptr) {
        return std::unexpected(StorageManagerError{
            StorageManagerErrorCode::primary_key_not_found});
    }
    if (metadata->root_page_id == tree->second.root_page_id()) {
        return {};
    }
    StoredIndexMetadata updated = *metadata;
    updated.root_page_id = tree->second.root_page_id();
    auto persisted = catalog_store.update_index_root(table_id, updated);
    if (!persisted) {
        return std::unexpected(StorageManagerError{persisted.error()});
    }
    *metadata = std::move(updated);
    return {};
}

}  // namespace

struct StorageManager::State {
    State(DatabaseFile file, std::size_t capacity)
        : database_file(std::move(file)),
          buffer_pool(database_file, capacity) {}

    DatabaseFile database_file;
    BufferPool buffer_pool;
    std::optional<CatalogStore> catalog_store;
    std::vector<StoredTableMetadata> tables;
    std::unordered_map<std::uint64_t, TableHeap> table_heaps;
    std::unordered_map<std::uint64_t, index::BTree> primary_indexes;
};

std::expected<StorageManager, StorageManagerError> StorageManager::create(
    const std::filesystem::path& path, std::size_t buffer_pool_capacity) {
    if (buffer_pool_capacity == 0) {
        return std::unexpected(
            StorageManagerError{StorageManagerErrorCode::invalid_buffer_pool_capacity});
    }

    auto database_file = DatabaseFile::create(path);
    if (!database_file) {
        return std::unexpected(StorageManagerError{database_file.error()});
    }
    auto state =
        std::make_unique<State>(std::move(*database_file), buffer_pool_capacity);
    auto catalog_store =
        CatalogStore::create(state->database_file, state->buffer_pool);
    if (!catalog_store) {
        return std::unexpected(StorageManagerError{catalog_store.error()});
    }
    state->catalog_store.emplace(std::move(*catalog_store));
    return StorageManager{std::move(state)};
}

std::expected<StorageManager, StorageManagerError> StorageManager::open(
    const std::filesystem::path& path, std::size_t buffer_pool_capacity) {
    if (buffer_pool_capacity == 0) {
        return std::unexpected(
            StorageManagerError{StorageManagerErrorCode::invalid_buffer_pool_capacity});
    }

    auto database_file = DatabaseFile::open(path);
    if (!database_file) {
        return std::unexpected(StorageManagerError{database_file.error()});
    }
    auto state =
        std::make_unique<State>(std::move(*database_file), buffer_pool_capacity);
    auto catalog_store =
        CatalogStore::open(state->database_file, state->buffer_pool);
    if (!catalog_store) {
        return std::unexpected(StorageManagerError{catalog_store.error()});
    }
    auto tables = catalog_store->load_tables();
    if (!tables) {
        return std::unexpected(StorageManagerError{tables.error()});
    }
    for (const auto& table : *tables) {
        auto table_heap = TableHeap::open(
            state->buffer_pool, table.schema, table.first_page_id);
        if (!table_heap) {
            return std::unexpected(StorageManagerError{table_heap.error()});
        }
        state->table_heaps.emplace(table.schema.id.value,
                                   std::move(*table_heap));
        if (const auto* stored_index = primary_index(table)) {
            auto tree = index::BTree::open(
                state->buffer_pool, stored_index->root_page_id);
            if (!tree) {
                return std::unexpected(
                    StorageManagerError{tree.error()});
            }
            state->primary_indexes.emplace(table.schema.id.value,
                                           std::move(*tree));
        }
    }
    state->tables = std::move(*tables);
    state->catalog_store.emplace(std::move(*catalog_store));
    return StorageManager{std::move(state)};
}

StorageManager::StorageManager(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

StorageManager::StorageManager(StorageManager&&) noexcept = default;

StorageManager& StorageManager::operator=(StorageManager&&) noexcept = default;

StorageManager::~StorageManager() = default;

const std::filesystem::path& StorageManager::path() const noexcept {
    return state_->database_file.path();
}

std::size_t StorageManager::buffer_pool_capacity() const noexcept {
    return state_->buffer_pool.capacity();
}

std::uint64_t StorageManager::page_count() const noexcept {
    return state_->database_file.header().page_count;
}

std::span<const StoredTableMetadata> StorageManager::tables() const noexcept {
    return state_->tables;
}

std::expected<StoredTableMetadata, StorageManagerError>
StorageManager::create_table(catalog::TableSchema schema) {
    auto valid = state_->catalog_store->validate_table(schema);
    if (!valid) {
        return std::unexpected(StorageManagerError{valid.error()});
    }

    auto table_heap = TableHeap::create(state_->buffer_pool, schema);
    if (!table_heap) {
        return std::unexpected(StorageManagerError{table_heap.error()});
    }
    const common::PageId first_page_id = table_heap->first_page_id();
    std::optional<index::BTree> primary_tree;
    std::vector<StoredIndexMetadata> indexes;
    if (const auto column_index = primary_key_column(schema)) {
        auto tree = index::BTree::create(state_->buffer_pool);
        if (!tree) {
            return std::unexpected(StorageManagerError{tree.error()});
        }
        indexes.push_back(StoredIndexMetadata{
            schema.name + "_primary_key", *column_index,
            tree->root_page_id(), true, true});
        primary_tree.emplace(std::move(*tree));
    }
    auto stored = state_->catalog_store->add_table(
        schema, first_page_id, indexes);
    if (!stored) {
        return std::unexpected(StorageManagerError{stored.error()});
    }

    state_->table_heaps.emplace(schema.id.value, std::move(*table_heap));
    if (primary_tree) {
        state_->primary_indexes.emplace(schema.id.value,
                                        std::move(*primary_tree));
    }
    StoredTableMetadata metadata{
        std::move(schema), first_page_id, std::move(indexes)};
    state_->tables.push_back(metadata);
    return metadata;
}

std::expected<RecordId, StorageManagerError> StorageManager::insert(
    common::TableId table_id, const Row& row) {
    const auto table = state_->table_heaps.find(table_id.value);
    if (!table_id.is_valid() || table == state_->table_heaps.end()) {
        return std::unexpected(
            StorageManagerError{StorageManagerErrorCode::table_not_found});
    }

    const auto* metadata = find_table_metadata(state_->tables, table_id);
    if (metadata == nullptr || row.values.size() != metadata->schema.columns.size()) {
        return std::unexpected(StorageManagerError{
            StorageManagerErrorCode::invalid_row_shape});
    }
    std::optional<index::EncodedKey> key;
    auto primary = state_->primary_indexes.find(table_id.value);
    if (const auto column_index = primary_key_column(metadata->schema)) {
        auto encoded = encode_primary_key(row, *column_index);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        key = std::move(*encoded);
        if (primary == state_->primary_indexes.end()) {
            return std::unexpected(StorageManagerError{
                StorageManagerErrorCode::primary_key_not_found});
        }
        auto matches = primary->second.find(*key);
        if (!matches) {
            return std::unexpected(StorageManagerError{matches.error()});
        }
        if (!matches->empty()) {
            return std::unexpected(StorageManagerError{
                StorageManagerErrorCode::unique_constraint_violation});
        }
    }

    auto record_id = table->second.insert(row);
    if (!record_id) {
        return std::unexpected(StorageManagerError{record_id.error()});
    }
    if (key) {
        auto indexed = primary->second.insert(*key, *record_id);
        if (!indexed) {
            (void)table->second.erase(*record_id);
            return std::unexpected(StorageManagerError{indexed.error()});
        }
        auto root_persisted = persist_primary_root(
            table_id, state_->tables, state_->primary_indexes,
            *state_->catalog_store);
        if (!root_persisted) {
            (void)primary->second.erase(*key, *record_id);
            (void)table->second.erase(*record_id);
            return std::unexpected(root_persisted.error());
        }
    }
    auto flushed = state_->buffer_pool.flush();
    if (!flushed) {
        return std::unexpected(StorageManagerError{flushed.error()});
    }
    return *record_id;
}

std::expected<void, StorageManagerError> StorageManager::update(
    common::TableId table_id, RecordId record_id, const Row& row) {
    const auto table = state_->table_heaps.find(table_id.value);
    if (!table_id.is_valid() || table == state_->table_heaps.end()) {
        return std::unexpected(
            StorageManagerError{StorageManagerErrorCode::table_not_found});
    }

    const auto* metadata = find_table_metadata(state_->tables, table_id);
    if (metadata == nullptr ||
        row.values.size() != metadata->schema.columns.size()) {
        return std::unexpected(StorageManagerError{
            StorageManagerErrorCode::invalid_row_shape});
    }
    auto before = table->second.read(record_id);
    if (!before) {
        return std::unexpected(StorageManagerError{before.error()});
    }

    std::optional<index::EncodedKey> old_key;
    std::optional<index::EncodedKey> new_key;
    auto primary = state_->primary_indexes.find(table_id.value);
    if (const auto column_index = primary_key_column(metadata->schema)) {
        auto old_encoded = encode_primary_key(*before, *column_index);
        auto new_encoded = encode_primary_key(row, *column_index);
        if (!old_encoded) {
            return std::unexpected(old_encoded.error());
        }
        if (!new_encoded) {
            return std::unexpected(new_encoded.error());
        }
        old_key = std::move(*old_encoded);
        new_key = std::move(*new_encoded);
        if (primary == state_->primary_indexes.end()) {
            return std::unexpected(StorageManagerError{
                StorageManagerErrorCode::primary_key_not_found});
        }
        if (*old_key != *new_key) {
            auto matches = primary->second.find(*new_key);
            if (!matches) {
                return std::unexpected(StorageManagerError{matches.error()});
            }
            if (!matches->empty()) {
                return std::unexpected(StorageManagerError{
                    StorageManagerErrorCode::unique_constraint_violation});
            }
            auto erased = primary->second.erase(*old_key, record_id);
            if (!erased) {
                return std::unexpected(StorageManagerError{erased.error()});
            }
            auto inserted = primary->second.insert(*new_key, record_id);
            if (!inserted) {
                (void)primary->second.insert(*old_key, record_id);
                return std::unexpected(StorageManagerError{inserted.error()});
            }
        }
    }

    auto updated = table->second.update(record_id, row);
    if (!updated) {
        if (old_key && new_key && *old_key != *new_key) {
            (void)primary->second.erase(*new_key, record_id);
            (void)primary->second.insert(*old_key, record_id);
        }
        return std::unexpected(StorageManagerError{updated.error()});
    }
    if (old_key && new_key && *old_key != *new_key) {
        auto root_persisted = persist_primary_root(
            table_id, state_->tables, state_->primary_indexes,
            *state_->catalog_store);
        if (!root_persisted) {
            (void)table->second.update(record_id, *before);
            (void)primary->second.erase(*new_key, record_id);
            (void)primary->second.insert(*old_key, record_id);
            return std::unexpected(root_persisted.error());
        }
    }
    auto flushed = state_->buffer_pool.flush();
    if (!flushed) {
        return std::unexpected(StorageManagerError{flushed.error()});
    }
    return {};
}

std::expected<void, StorageManagerError> StorageManager::erase(
    common::TableId table_id, RecordId record_id) {
    const auto table = state_->table_heaps.find(table_id.value);
    if (!table_id.is_valid() || table == state_->table_heaps.end()) {
        return std::unexpected(
            StorageManagerError{StorageManagerErrorCode::table_not_found});
    }

    const auto* metadata = find_table_metadata(state_->tables, table_id);
    if (metadata == nullptr) {
        return std::unexpected(StorageManagerError{
            StorageManagerErrorCode::table_not_found});
    }
    auto row = table->second.read(record_id);
    if (!row) {
        return std::unexpected(StorageManagerError{row.error()});
    }
    std::optional<index::EncodedKey> key;
    auto primary = state_->primary_indexes.find(table_id.value);
    if (const auto column_index = primary_key_column(metadata->schema)) {
        auto encoded = encode_primary_key(*row, *column_index);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        key = std::move(*encoded);
        if (primary == state_->primary_indexes.end()) {
            return std::unexpected(StorageManagerError{
                StorageManagerErrorCode::primary_key_not_found});
        }
        auto index_erased = primary->second.erase(*key, record_id);
        if (!index_erased) {
            return std::unexpected(
                StorageManagerError{index_erased.error()});
        }
    }

    auto erased = table->second.erase(record_id);
    if (!erased) {
        if (key) {
            (void)primary->second.insert(*key, record_id);
        }
        return std::unexpected(StorageManagerError{erased.error()});
    }
    if (key) {
        auto root_persisted = persist_primary_root(
            table_id, state_->tables, state_->primary_indexes,
            *state_->catalog_store);
        if (!root_persisted) {
            (void)table->second.restore(record_id, *row);
            (void)primary->second.insert(*key, record_id);
            return std::unexpected(root_persisted.error());
        }
    }
    auto flushed = state_->buffer_pool.flush();
    if (!flushed) {
        return std::unexpected(StorageManagerError{flushed.error()});
    }
    return {};
}

std::expected<void, StorageManagerError> StorageManager::restore(
    common::TableId table_id, RecordId record_id, const Row& row) {
    const auto table = state_->table_heaps.find(table_id.value);
    if (!table_id.is_valid() || table == state_->table_heaps.end()) {
        return std::unexpected(
            StorageManagerError{StorageManagerErrorCode::table_not_found});
    }

    const auto* metadata = find_table_metadata(state_->tables, table_id);
    if (metadata == nullptr ||
        row.values.size() != metadata->schema.columns.size()) {
        return std::unexpected(StorageManagerError{
            StorageManagerErrorCode::invalid_row_shape});
    }
    std::optional<index::EncodedKey> key;
    auto primary = state_->primary_indexes.find(table_id.value);
    if (const auto column_index = primary_key_column(metadata->schema)) {
        auto encoded = encode_primary_key(row, *column_index);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        key = std::move(*encoded);
        if (primary == state_->primary_indexes.end()) {
            return std::unexpected(StorageManagerError{
                StorageManagerErrorCode::primary_key_not_found});
        }
        auto matches = primary->second.find(*key);
        if (!matches) {
            return std::unexpected(StorageManagerError{matches.error()});
        }
        if (!matches->empty()) {
            return std::unexpected(StorageManagerError{
                StorageManagerErrorCode::unique_constraint_violation});
        }
    }

    auto restored = table->second.restore(record_id, row);
    if (!restored) {
        return std::unexpected(StorageManagerError{restored.error()});
    }
    if (key) {
        auto indexed = primary->second.insert(*key, record_id);
        if (!indexed) {
            (void)table->second.erase(record_id);
            return std::unexpected(StorageManagerError{indexed.error()});
        }
        auto root_persisted = persist_primary_root(
            table_id, state_->tables, state_->primary_indexes,
            *state_->catalog_store);
        if (!root_persisted) {
            (void)primary->second.erase(*key, record_id);
            (void)table->second.erase(record_id);
            return std::unexpected(root_persisted.error());
        }
    }
    auto flushed = state_->buffer_pool.flush();
    if (!flushed) {
        return std::unexpected(StorageManagerError{flushed.error()});
    }
    return {};
}

std::expected<std::vector<Row>, StorageManagerError> StorageManager::scan(
    common::TableId table_id) const {
    const auto table = state_->table_heaps.find(table_id.value);
    if (!table_id.is_valid() || table == state_->table_heaps.end()) {
        return std::unexpected(
            StorageManagerError{StorageManagerErrorCode::table_not_found});
    }

    auto rows = table->second.scan();
    if (!rows) {
        return std::unexpected(StorageManagerError{rows.error()});
    }
    return rows;
}

std::expected<std::vector<StoredRow>, StorageManagerError>
StorageManager::scan_records(common::TableId table_id) const {
    const auto table = state_->table_heaps.find(table_id.value);
    if (!table_id.is_valid() || table == state_->table_heaps.end()) {
        return std::unexpected(
            StorageManagerError{StorageManagerErrorCode::table_not_found});
    }

    auto records = table->second.scan_records();
    if (!records) {
        return std::unexpected(StorageManagerError{records.error()});
    }
    return records;
}

std::expected<std::vector<Row>, StorageManagerError>
StorageManager::lookup_primary_key(
    common::TableId table_id, const types::Value& value) const {
    const auto table = state_->table_heaps.find(table_id.value);
    const auto tree = state_->primary_indexes.find(table_id.value);
    const auto* metadata = find_table_metadata(state_->tables, table_id);
    if (!table_id.is_valid() || table == state_->table_heaps.end() ||
        metadata == nullptr) {
        return std::unexpected(
            StorageManagerError{StorageManagerErrorCode::table_not_found});
    }
    const auto column_index = primary_key_column(metadata->schema);
    if (!column_index || tree == state_->primary_indexes.end()) {
        return std::unexpected(StorageManagerError{
            StorageManagerErrorCode::primary_key_not_found});
    }
    if (std::holds_alternative<types::NullValue>(value)) {
        return std::vector<Row>{};
    }
    const auto& column = metadata->schema.columns[*column_index];
    const bool type_matches =
        (column.type == types::LogicalType::integer &&
         std::holds_alternative<std::int64_t>(value)) ||
        (column.type == types::LogicalType::text &&
         std::holds_alternative<std::string>(value));
    if (!type_matches) {
        return std::unexpected(StorageManagerError{
            StorageManagerErrorCode::invalid_row_shape});
    }

    const auto key = index::encode_key(
        std::span<const types::Value>{&value, 1});
    auto matches = tree->second.find(key);
    if (!matches) {
        return std::unexpected(StorageManagerError{matches.error()});
    }
    std::vector<Row> rows;
    rows.reserve(matches->size());
    for (const auto record_id : *matches) {
        auto row = table->second.read(record_id);
        if (!row) {
            return std::unexpected(StorageManagerError{row.error()});
        }
        rows.push_back(std::move(*row));
    }
    return rows;
}

}  // namespace minidb::storage
