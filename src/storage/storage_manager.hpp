#pragma once

#include "storage/catalog/catalog_store.hpp"
#include "storage/io/database_file.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <variant>

namespace minidb::storage {

inline constexpr std::size_t default_buffer_pool_capacity = 64;

enum class StorageManagerErrorCode {
    invalid_buffer_pool_capacity,
};

using StorageManagerError =
    std::variant<DatabaseFileError, CatalogStoreError, StorageManagerErrorCode>;

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

private:
    struct State;

    explicit StorageManager(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;
};

}  // namespace minidb::storage
