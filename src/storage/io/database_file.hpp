#pragma once

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
    page_id_mismatch,
};

using DatabaseFileError = std::variant<PageFileError, PageChecksumError,
                                       DatabaseHeaderError, SlottedPageError,
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
    [[nodiscard]] std::expected<common::PageId, DatabaseFileError> allocate_heap_page();
    [[nodiscard]] std::expected<PageBuffer, DatabaseFileError> read_heap_page(
        common::PageId page_id);
    [[nodiscard]] std::expected<void, DatabaseFileError> write_heap_page(
        common::PageId page_id, const PageBuffer& page);

private:
    DatabaseFile(PageFile page_file, DatabaseHeader header);

    PageFile page_file_;
    DatabaseHeader header_;
};

}  // namespace minidb::storage
