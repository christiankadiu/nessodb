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

std::expected<void, DatabaseFileError> DatabaseFile::initialize_catalog_root(
    common::PageId page_id) {
    if (header_.catalog_root) {
        return std::unexpected(DatabaseFileError{
            DatabaseFileStructureError::catalog_root_already_initialized});
    }

    DatabaseHeader updated_header = header_;
    updated_header.catalog_root = page_id;
    auto header_page = make_header_page(updated_header);
    if (!header_page) {
        return std::unexpected(header_page.error());
    }
    auto written = page_file_.write_page(common::PageId{0}, *header_page);
    if (!written) {
        return std::unexpected(DatabaseFileError{written.error()});
    }
    auto flushed = page_file_.flush();
    if (!flushed) {
        return std::unexpected(DatabaseFileError{flushed.error()});
    }
    header_ = updated_header;
    return {};
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
    return append_page(std::move(heap_page));
}

std::expected<common::PageId, DatabaseFileError>
DatabaseFile::allocate_index_leaf_page() {
    if (header_.page_count == common::PageId::invalid_value) {
        return std::unexpected(
            DatabaseFileError{DatabaseFileStructureError::page_id_exhausted});
    }
    const common::PageId page_id{header_.page_count};
    PageBuffer page{};
    auto encoded = index::encode_leaf_page(
        page, index::LeafPage{page_id, std::nullopt, std::nullopt, {}});
    if (!encoded) {
        return std::unexpected(DatabaseFileError{encoded.error()});
    }
    return append_page(std::move(page));
}

std::expected<common::PageId, DatabaseFileError>
DatabaseFile::allocate_index_internal_page(
    std::uint16_t level, common::PageId leftmost_child) {
    if (header_.page_count == common::PageId::invalid_value) {
        return std::unexpected(
            DatabaseFileError{DatabaseFileStructureError::page_id_exhausted});
    }
    if (!leftmost_child.is_valid() || leftmost_child.value == 0 ||
        leftmost_child.value >= header_.page_count) {
        return std::unexpected(DatabaseFileError{
            DatabaseFileStructureError::invalid_index_page_id});
    }
    const common::PageId page_id{header_.page_count};
    PageBuffer page{};
    auto encoded = index::encode_internal_page(
        page, index::InternalPage{page_id, std::nullopt, level,
                                  leftmost_child, {}});
    if (!encoded) {
        return std::unexpected(DatabaseFileError{encoded.error()});
    }
    return append_page(std::move(page));
}

std::expected<common::PageId, DatabaseFileError> DatabaseFile::append_page(
    PageBuffer page) {
    if (header_.page_count == common::PageId::invalid_value) {
        return std::unexpected(
            DatabaseFileError{DatabaseFileStructureError::page_id_exhausted});
    }
    const common::PageId page_id{header_.page_count};
    auto page_header = decode_page_header(page);
    if (!page_header || page_header->type == PageType::database_header) {
        return std::unexpected(
            DatabaseFileError{DatabaseFileStructureError::invalid_data_page_id});
    }
    if (page_header->page_id != page_id) {
        return std::unexpected(
            DatabaseFileError{DatabaseFileStructureError::page_id_mismatch});
    }
    auto checksum = update_page_checksum(page);
    if (!checksum) {
        return std::unexpected(DatabaseFileError{checksum.error()});
    }

    DatabaseHeader updated_header = header_;
    ++updated_header.page_count;
    auto header_page = make_header_page(updated_header);
    if (!header_page) {
        return std::unexpected(header_page.error());
    }
    auto page_written = page_file_.write_page(page_id, page);
    if (!page_written) {
        return std::unexpected(DatabaseFileError{page_written.error()});
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

std::expected<PageBuffer, DatabaseFileError> DatabaseFile::read_page(
    common::PageId page_id) {
    if (!page_id.is_valid() || page_id.value == 0 || page_id.value >= header_.page_count) {
        return std::unexpected(
            DatabaseFileError{DatabaseFileStructureError::invalid_data_page_id});
    }

    auto page = page_file_.read_page(page_id);
    if (!page) {
        return std::unexpected(DatabaseFileError{page.error()});
    }
    auto checksum = verify_page_checksum(*page);
    if (!checksum) {
        return std::unexpected(DatabaseFileError{checksum.error()});
    }
    auto header = decode_page_header(*page);
    if (!header || header->type == PageType::database_header) {
        return std::unexpected(
            DatabaseFileError{DatabaseFileStructureError::invalid_data_page_id});
    }
    if (header->page_id != page_id) {
        return std::unexpected(
            DatabaseFileError{DatabaseFileStructureError::page_id_mismatch});
    }
    if (header->type == PageType::heap) {
        auto decoded = SlottedPage::open(*page);
        if (!decoded) {
            return std::unexpected(DatabaseFileError{decoded.error()});
        }
    } else if (header->type == PageType::index_leaf) {
        auto decoded = index::decode_leaf_page(*page);
        if (!decoded) {
            return std::unexpected(DatabaseFileError{decoded.error()});
        }
    } else if (header->type == PageType::index_internal) {
        auto decoded = index::decode_internal_page(*page);
        if (!decoded) {
            return std::unexpected(DatabaseFileError{decoded.error()});
        }
    }
    return page;
}

std::expected<void, DatabaseFileError> DatabaseFile::write_page(
    common::PageId page_id, const PageBuffer& page) {
    if (!page_id.is_valid() || page_id.value == 0 || page_id.value >= header_.page_count) {
        return std::unexpected(
            DatabaseFileError{DatabaseFileStructureError::invalid_data_page_id});
    }

    PageBuffer page_to_write = page;
    auto header = decode_page_header(page_to_write);
    if (!header || header->type == PageType::database_header) {
        return std::unexpected(
            DatabaseFileError{DatabaseFileStructureError::invalid_data_page_id});
    }
    if (header->page_id != page_id) {
        return std::unexpected(
            DatabaseFileError{DatabaseFileStructureError::page_id_mismatch});
    }
    if (header->type == PageType::heap) {
        auto decoded = SlottedPage::open(page_to_write);
        if (!decoded) {
            return std::unexpected(DatabaseFileError{decoded.error()});
        }
    } else if (header->type == PageType::index_leaf) {
        auto decoded = index::decode_leaf_page(page_to_write);
        if (!decoded) {
            return std::unexpected(DatabaseFileError{decoded.error()});
        }
    } else if (header->type == PageType::index_internal) {
        auto decoded = index::decode_internal_page(page_to_write);
        if (!decoded) {
            return std::unexpected(DatabaseFileError{decoded.error()});
        }
    }
    auto checksum = update_page_checksum(page_to_write);
    if (!checksum) {
        return std::unexpected(DatabaseFileError{checksum.error()});
    }
    auto written = page_file_.write_page(page_id, page_to_write);
    if (!written) {
        return std::unexpected(DatabaseFileError{written.error()});
    }
    auto flushed = page_file_.flush();
    if (!flushed) {
        return std::unexpected(DatabaseFileError{flushed.error()});
    }
    return {};
}

std::expected<PageBuffer, DatabaseFileError> DatabaseFile::read_heap_page(
    common::PageId page_id) {
    if (!page_id.is_valid() || page_id.value == 0 ||
        page_id.value >= header_.page_count) {
        return std::unexpected(DatabaseFileError{
            DatabaseFileStructureError::invalid_heap_page_id});
    }
    auto page = read_page(page_id);
    if (!page) {
        return std::unexpected(page.error());
    }
    auto decoded = SlottedPage::open(*page);
    if (!decoded) {
        return std::unexpected(DatabaseFileError{decoded.error()});
    }
    return page;
}

std::expected<void, DatabaseFileError> DatabaseFile::write_heap_page(
    common::PageId page_id, const PageBuffer& page) {
    if (!page_id.is_valid() || page_id.value == 0 ||
        page_id.value >= header_.page_count) {
        return std::unexpected(DatabaseFileError{
            DatabaseFileStructureError::invalid_heap_page_id});
    }
    PageBuffer copy = page;
    auto decoded = SlottedPage::open(copy);
    if (!decoded) {
        return std::unexpected(DatabaseFileError{decoded.error()});
    }
    return write_page(page_id, page);
}

std::expected<PageBuffer, DatabaseFileError> DatabaseFile::read_index_page(
    common::PageId page_id) {
    if (!page_id.is_valid() || page_id.value == 0 ||
        page_id.value >= header_.page_count) {
        return std::unexpected(DatabaseFileError{
            DatabaseFileStructureError::invalid_index_page_id});
    }
    auto page = read_page(page_id);
    if (!page) {
        return std::unexpected(page.error());
    }
    const auto header = decode_page_header(*page);
    if (!header || (header->type != PageType::index_leaf &&
                    header->type != PageType::index_internal)) {
        return std::unexpected(
            DatabaseFileError{index::IndexPageError::wrong_page_type});
    }
    return page;
}

std::expected<void, DatabaseFileError> DatabaseFile::write_index_page(
    common::PageId page_id, const PageBuffer& page) {
    if (!page_id.is_valid() || page_id.value == 0 ||
        page_id.value >= header_.page_count) {
        return std::unexpected(DatabaseFileError{
            DatabaseFileStructureError::invalid_index_page_id});
    }
    const auto header = decode_page_header(page);
    if (!header || (header->type != PageType::index_leaf &&
                    header->type != PageType::index_internal)) {
        return std::unexpected(
            DatabaseFileError{index::IndexPageError::wrong_page_type});
    }
    return write_page(page_id, page);
}

}  // namespace minidb::storage
