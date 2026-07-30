#pragma once

#include <expected>
#include <filesystem>
#include <fstream>

namespace minidb::storage {

enum class PageFileError {
    empty_path,
    path_not_found,
    path_already_exists,
    not_regular_file,
    open_failed,
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

private:
    PageFile(std::filesystem::path path, std::fstream stream);

    std::filesystem::path path_;
    std::fstream stream_;
};

}  // namespace minidb::storage
