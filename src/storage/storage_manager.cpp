#include "storage/storage_manager.hpp"

#include "storage/buffer/buffer_pool.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace minidb::storage {

struct StorageManager::State {
    State(DatabaseFile file, std::size_t capacity)
        : database_file(std::move(file)),
          buffer_pool(database_file, capacity) {}

    DatabaseFile database_file;
    BufferPool buffer_pool;
    std::optional<CatalogStore> catalog_store;
    std::vector<StoredTableMetadata> tables;
    std::unordered_map<std::uint64_t, TableHeap> table_heaps;
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

std::span<const StoredTableMetadata> StorageManager::tables() const noexcept {
    return state_->tables;
}

}  // namespace minidb::storage
