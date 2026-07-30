#include "storage/io/database_file.hpp"

#include "common/page_id.hpp"
#include "storage/page/page.hpp"

#include <utility>

namespace minidb::storage {

std::expected<DatabaseFile, DatabaseFileError> DatabaseFile::create(
    const std::filesystem::path& path) {
    auto page_file = PageFile::create(path);
    if (!page_file) {
        return std::unexpected(DatabaseFileError{page_file.error()});
    }

    DatabaseHeader header;
    PageBuffer header_page{};
    auto encoded = encode_database_header(header_page, header);
    if (!encoded) {
        return std::unexpected(DatabaseFileError{encoded.error()});
    }
    auto checksummed = update_page_checksum(header_page);
    if (!checksummed) {
        return std::unexpected(DatabaseFileError{checksummed.error()});
    }
    auto written = page_file->write_page(common::PageId{0}, header_page);
    if (!written) {
        return std::unexpected(DatabaseFileError{written.error()});
    }
    auto flushed = page_file->flush();
    if (!flushed) {
        return std::unexpected(DatabaseFileError{flushed.error()});
    }
    return DatabaseFile(std::move(*page_file), header);
}

std::expected<DatabaseFile, DatabaseFileError> DatabaseFile::open(
    const std::filesystem::path& path) {
    auto page_file = PageFile::open(path);
    if (!page_file) {
        return std::unexpected(DatabaseFileError{page_file.error()});
    }

    auto header_page = page_file->read_page(common::PageId{0});
    if (!header_page) {
        return std::unexpected(DatabaseFileError{header_page.error()});
    }
    auto checksum = verify_page_checksum(*header_page);
    if (!checksum) {
        return std::unexpected(DatabaseFileError{checksum.error()});
    }
    auto header = decode_database_header(*header_page);
    if (!header) {
        return std::unexpected(DatabaseFileError{header.error()});
    }

    auto physical_page_count = page_file->page_count();
    if (!physical_page_count) {
        return std::unexpected(DatabaseFileError{physical_page_count.error()});
    }
    if (*physical_page_count != header->page_count) {
        return std::unexpected(
            DatabaseFileError{DatabaseFileStructureError::page_count_mismatch});
    }
    return DatabaseFile(std::move(*page_file), *header);
}

DatabaseFile::DatabaseFile(PageFile page_file, DatabaseHeader header)
    : page_file_(std::move(page_file)), header_(header) {}

const std::filesystem::path& DatabaseFile::path() const noexcept {
    return page_file_.path();
}

const DatabaseHeader& DatabaseFile::header() const noexcept {
    return header_;
}

}  // namespace minidb::storage
