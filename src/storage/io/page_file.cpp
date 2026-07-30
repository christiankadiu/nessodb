#include "storage/io/page_file.hpp"

#include <system_error>
#include <utility>

namespace minidb::storage {
namespace {

std::filesystem::file_status get_status(const std::filesystem::path& path,
                                        std::error_code& error) noexcept {
    return std::filesystem::symlink_status(path, error);
}

}  // namespace

std::expected<PageFile, PageFileError> PageFile::create(
    const std::filesystem::path& path) {
    if (path.empty()) {
        return std::unexpected(PageFileError::empty_path);
    }

    std::error_code status_error;
    const auto status = get_status(path, status_error);
    if (status_error && status_error != std::errc::no_such_file_or_directory) {
        return std::unexpected(PageFileError::open_failed);
    }
    if (status.type() != std::filesystem::file_type::not_found) {
        return std::unexpected(PageFileError::path_already_exists);
    }

    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out |
                                  std::ios::trunc);
    if (!stream.is_open()) {
        return std::unexpected(PageFileError::open_failed);
    }
    return PageFile(path, std::move(stream));
}

std::expected<PageFile, PageFileError> PageFile::open(
    const std::filesystem::path& path) {
    if (path.empty()) {
        return std::unexpected(PageFileError::empty_path);
    }

    std::error_code status_error;
    const auto status = get_status(path, status_error);
    if (status_error) {
        if (status_error == std::errc::no_such_file_or_directory) {
            return std::unexpected(PageFileError::path_not_found);
        }
        return std::unexpected(PageFileError::open_failed);
    }
    if (status.type() == std::filesystem::file_type::not_found) {
        return std::unexpected(PageFileError::path_not_found);
    }
    if (status.type() != std::filesystem::file_type::regular) {
        return std::unexpected(PageFileError::not_regular_file);
    }

    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream.is_open()) {
        return std::unexpected(PageFileError::open_failed);
    }
    return PageFile(path, std::move(stream));
}

PageFile::PageFile(std::filesystem::path path, std::fstream stream)
    : path_(std::move(path)), stream_(std::move(stream)) {}

bool PageFile::is_open() const noexcept {
    return stream_.is_open();
}

const std::filesystem::path& PageFile::path() const noexcept {
    return path_;
}

}  // namespace minidb::storage
