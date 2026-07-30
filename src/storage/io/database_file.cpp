#include "storage/io/database_file.hpp"

#include "common/page_id.hpp"
#include "storage/page/page.hpp"

#include <utility>

namespace minidb::storage {
namespace {

std::expected<PageBuffer, DatabaseFileError> make_header_page(
    const DatabaseHeader& header) noexcept {
    PageBuffer page{};
    auto encoded = encode_database_header(page, header);
    if (!encoded) {
        return std::unexpected(DatabaseFileError{encoded.error()});
    }
    auto checksummed = update_page_checksum(page);
    if (!checksummed) {
        return std::unexpected(DatabaseFileError{checksummed.error()});
    }
    return page;
}

}  // namespace

std::expected<DatabaseFile, DatabaseFileError> DatabaseFile::create(
    const std::filesystem::path& path) {
    auto page_file = PageFile::create(path);
    if (!page_file) {
        return std::unexpected(DatabaseFileError{page_file.error()});
    }

    DatabaseHeader header;
    auto header_page = make_header_page(header);
    if (!header_page) {
        return std::unexpected(header_page.error());
    }
    auto written = page_file->write_page(common::PageId{0}, *header_page);
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

std::expected<common::PageId, DatabaseFileError> DatabaseFile::allocate_heap_page() {
    if (header_.page_count == common::PageId::invalid_value) {
        return std::unexpected(
            DatabaseFileError{DatabaseFileStructureError::page_id_exhausted});
    }

    const common::PageId page_id{header_.page_count};
    PageBuffer heap_page{};
    auto initialized = SlottedPage::initialize(heap_page, page_id);
    if (!initialized) {
        return std::unexpected(DatabaseFileError{initialized.error()});
    }
    auto heap_checksum = update_page_checksum(heap_page);
    if (!heap_checksum) {
        return std::unexpected(DatabaseFileError{heap_checksum.error()});
    }

    DatabaseHeader updated_header = header_;
    ++updated_header.page_count;
    auto header_page = make_header_page(updated_header);
    if (!header_page) {
        return std::unexpected(header_page.error());
    }

    auto heap_written = page_file_.write_page(page_id, heap_page);
    if (!heap_written) {
        return std::unexpected(DatabaseFileError{heap_written.error()});
    }
    auto header_written = page_file_.write_page(common::PageId{0}, *header_page);
    if (!header_written) {
        return std::unexpected(DatabaseFileError{header_written.error()});
    }
    header_ = updated_header;

    auto flushed = page_file_.flush();
    if (!flushed) {
        return std::unexpected(DatabaseFileError{flushed.error()});
    }
    return page_id;
}

}  // namespace minidb::storage
