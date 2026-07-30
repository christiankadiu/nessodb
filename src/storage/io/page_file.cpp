#include "storage/io/page_file.hpp"

#include <cstdint>
#include <limits>
#include <system_error>
#include <utility>

namespace minidb::storage {
namespace {

std::filesystem::file_status get_status(const std::filesystem::path& path,
                                        std::error_code& error) noexcept {
    return std::filesystem::symlink_status(path, error);
}

std::expected<std::streamoff, PageFileError> page_offset(common::PageId page_id) noexcept {
    if (!page_id.is_valid()) {
        return std::unexpected(PageFileError::invalid_page_id);
    }

    constexpr auto maximum_offset = std::numeric_limits<std::streamoff>::max();
    const auto maximum_page_id =
        static_cast<std::uint64_t>(maximum_offset) / static_cast<std::uint64_t>(page_size);
    if (page_id.value > maximum_page_id) {
        return std::unexpected(PageFileError::page_offset_out_of_range);
    }
    return static_cast<std::streamoff>(page_id.value * page_size);
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

std::expected<std::uint64_t, PageFileError> PageFile::page_count() {
    const auto size = file_size();
    if (!size) {
        return std::unexpected(size.error());
    }
    if (*size % static_cast<std::streamoff>(page_size) != 0) {
        return std::unexpected(PageFileError::invalid_file_size);
    }
    return static_cast<std::uint64_t>(*size / static_cast<std::streamoff>(page_size));
}

std::expected<PageBuffer, PageFileError> PageFile::read_page(common::PageId page_id) {
    const auto offset = page_offset(page_id);
    if (!offset) {
        return std::unexpected(offset.error());
    }

    const auto size = file_size();
    if (!size) {
        return std::unexpected(size.error());
    }
    if (*size % static_cast<std::streamoff>(page_size) != 0) {
        return std::unexpected(PageFileError::invalid_file_size);
    }
    if (*offset >= *size) {
        return std::unexpected(PageFileError::page_not_found);
    }

    stream_.clear();
    stream_.seekg(*offset);
    if (!stream_) {
        return std::unexpected(PageFileError::seek_failed);
    }

    PageBuffer page{};
    stream_.read(reinterpret_cast<char*>(page.data()),
                 static_cast<std::streamsize>(page.size()));
    if (!stream_) {
        return std::unexpected(PageFileError::read_failed);
    }
    return page;
}

std::expected<void, PageFileError> PageFile::write_page(common::PageId page_id,
                                                        const PageBuffer& page) {
    const auto offset = page_offset(page_id);
    if (!offset) {
        return std::unexpected(offset.error());
    }

    const auto size = file_size();
    if (!size) {
        return std::unexpected(size.error());
    }
    if (*size % static_cast<std::streamoff>(page_size) != 0) {
        return std::unexpected(PageFileError::invalid_file_size);
    }
    if (*offset > *size) {
        return std::unexpected(PageFileError::non_contiguous_write);
    }

    stream_.clear();
    stream_.seekp(*offset);
    if (!stream_) {
        return std::unexpected(PageFileError::seek_failed);
    }
    stream_.write(reinterpret_cast<const char*>(page.data()),
                  static_cast<std::streamsize>(page.size()));
    if (!stream_) {
        return std::unexpected(PageFileError::write_failed);
    }
    return {};
}

std::expected<void, PageFileError> PageFile::flush() {
    stream_.flush();
    if (!stream_) {
        return std::unexpected(PageFileError::write_failed);
    }
    return {};
}

std::expected<std::streamoff, PageFileError> PageFile::file_size() {
    stream_.clear();
    stream_.seekg(0, std::ios::end);
    if (!stream_) {
        return std::unexpected(PageFileError::seek_failed);
    }
    const auto size = stream_.tellg();
    if (size < 0) {
        return std::unexpected(PageFileError::seek_failed);
    }
    return size;
}

}  // namespace minidb::storage
