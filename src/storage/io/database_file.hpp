#pragma once

#include "index/btree_page.hpp"
#include "storage/io/page_file.hpp"
#include "storage/page/checksum.hpp"
#include "storage/page/database_header.hpp"
#include "storage/page/slotted_page.hpp"

#include <expected>
#include <filesystem>
#include <variant>

namespace minidb::storage {

enum class DatabaseFileStructureError {
    page_count_mismatch,
    page_id_exhausted,
    invalid_heap_page_id,
    invalid_index_page_id,
    invalid_data_page_id,
    page_id_mismatch,
    catalog_root_already_initialized,
};

using DatabaseFileError = std::variant<PageFileError, PageChecksumError,
                                       DatabaseHeaderError, SlottedPageError,
                                       index::IndexPageError,
                                       DatabaseFileStructureError>;

class DatabaseFile {
public:
    [[nodiscard]] static std::expected<DatabaseFile, DatabaseFileError> create(
        const std::filesystem::path& path);
    [[nodiscard]] static std::expected<DatabaseFile, DatabaseFileError> open(
        const std::filesystem::path& path);

    DatabaseFile(const DatabaseFile&) = delete;
    DatabaseFile& operator=(const DatabaseFile&) = delete;
    DatabaseFile(DatabaseFile&&) noexcept = default;
    DatabaseFile& operator=(DatabaseFile&&) noexcept = default;
    ~DatabaseFile() = default;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] const DatabaseHeader& header() const noexcept;
    [[nodiscard]] std::expected<void, DatabaseFileError> initialize_catalog_root(
        common::PageId page_id);
    [[nodiscard]] std::expected<common::PageId, DatabaseFileError> allocate_heap_page();
    [[nodiscard]] std::expected<common::PageId, DatabaseFileError>
    allocate_index_leaf_page();
    [[nodiscard]] std::expected<common::PageId, DatabaseFileError>
    allocate_index_internal_page(std::uint16_t level,
                                 common::PageId leftmost_child);
    [[nodiscard]] std::expected<PageBuffer, DatabaseFileError> read_page(
        common::PageId page_id);
    [[nodiscard]] std::expected<void, DatabaseFileError> write_page(
        common::PageId page_id, const PageBuffer& page);
    [[nodiscard]] std::expected<PageBuffer, DatabaseFileError> read_heap_page(
        common::PageId page_id);
    [[nodiscard]] std::expected<void, DatabaseFileError> write_heap_page(
        common::PageId page_id, const PageBuffer& page);
    [[nodiscard]] std::expected<PageBuffer, DatabaseFileError> read_index_page(
        common::PageId page_id);
    [[nodiscard]] std::expected<void, DatabaseFileError> write_index_page(
        common::PageId page_id, const PageBuffer& page);

private:
    DatabaseFile(PageFile page_file, DatabaseHeader header);
    [[nodiscard]] std::expected<common::PageId, DatabaseFileError>
    append_page(PageBuffer page);

    PageFile page_file_;
    DatabaseHeader header_;
};

}  // namespace minidb::storage
