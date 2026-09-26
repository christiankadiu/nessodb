#pragma once

#include "common/page_id.hpp"
#include "storage/page/page.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>

namespace nessodb::storage {

enum class PageFileError {
    empty_path,
    path_not_found,
    path_already_exists,
    not_regular_file,
    open_failed,
    invalid_page_id,
    page_offset_out_of_range,
    invalid_file_size,
    page_not_found,
    non_contiguous_write,
    seek_failed,
    read_failed,
    write_failed,
    resize_failed,
};

class PageFile {
public:
    [[nodiscard]] static std::expected<PageFile, PageFileError> create(
        const std::filesystem::path& path);
    [[nodiscard]] static std::expected<PageFile, PageFileError> open(
        const std::filesystem::path& path);

    PageFile(const PageFile&) = delete;
    PageFile& operator=(const PageFile&) = delete;
    PageFile(PageFile&&) noexcept = default;
    PageFile& operator=(PageFile&&) noexcept = default;
    ~PageFile() = default;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] std::expected<std::uint64_t, PageFileError> page_count();
    [[nodiscard]] std::expected<PageBuffer, PageFileError> read_page(
        common::PageId page_id);
    [[nodiscard]] std::expected<void, PageFileError> write_page(
        common::PageId page_id, const PageBuffer& page);
    [[nodiscard]] std::expected<void, PageFileError> flush();
    [[nodiscard]] std::expected<void, PageFileError> truncate_pages(
        std::uint64_t page_count);

private:
    PageFile(std::filesystem::path path, std::fstream stream);

    [[nodiscard]] std::expected<std::streamoff, PageFileError> file_size();

    std::filesystem::path path_;
    std::fstream stream_;
};

}  // namespace nessodb::storage
