#include "storage/page/page_header.hpp"

#include "storage/page/codec.hpp"

namespace nessodb::storage {
namespace {

inline constexpr std::size_t magic_offset = 0;
inline constexpr std::size_t version_offset = 4;
inline constexpr std::size_t type_offset = 6;
inline constexpr std::size_t page_id_offset = 8;
inline constexpr std::size_t lsn_offset = 16;
inline constexpr std::size_t reserved_offset = 28;

bool is_known_page_type(PageType type) noexcept {
    return type == PageType::database_header || type == PageType::heap ||
           type == PageType::index_leaf ||
           type == PageType::index_internal;
}

bool page_id_matches_type(PageType type, common::PageId page_id) noexcept {
    if (!page_id.is_valid()) {
        return false;
    }
    if (type == PageType::database_header) {
        return page_id.value == 0;
    }
    return page_id.value != 0;
}

}  // namespace

std::expected<void, PageHeaderError> encode_page_header(
    std::span<std::byte> destination, const PageHeader& header) noexcept {
    if (destination.size() < page_header_size) {
        return std::unexpected(PageHeaderError::page_too_small);
    }
    if (!is_known_page_type(header.type)) {
        return std::unexpected(PageHeaderError::unknown_page_type);
    }
    if (!page_id_matches_type(header.type, header.page_id)) {
        return std::unexpected(PageHeaderError::invalid_page_id);
    }

    const bool encoded = write_u32(destination, magic_offset, page_magic) &&
                         write_u16(destination, version_offset, page_format_version) &&
                         write_u16(destination, type_offset,
                                   static_cast<std::uint16_t>(header.type)) &&
                         write_u64(destination, page_id_offset, header.page_id.value) &&
                         write_u64(destination, lsn_offset, header.log_sequence_number) &&
                         write_u32(destination, page_checksum_offset, header.checksum) &&
                         write_u32(destination, reserved_offset, 0);
    if (!encoded) {
        return std::unexpected(PageHeaderError::page_too_small);
    }
    return {};
}

std::expected<PageHeader, PageHeaderError> decode_page_header(
    std::span<const std::byte> source) noexcept {
    if (source.size() < page_header_size) {
        return std::unexpected(PageHeaderError::page_too_small);
    }

    const std::uint32_t magic = *read_u32(source, magic_offset);
    if (magic != page_magic) {
        return std::unexpected(PageHeaderError::invalid_magic);
    }

    const std::uint16_t version = *read_u16(source, version_offset);
    if (version != page_format_version) {
        return std::unexpected(PageHeaderError::unsupported_version);
    }

    const auto type = static_cast<PageType>(*read_u16(source, type_offset));
    if (!is_known_page_type(type)) {
        return std::unexpected(PageHeaderError::unknown_page_type);
    }

    const common::PageId page_id{*read_u64(source, page_id_offset)};
    if (!page_id_matches_type(type, page_id)) {
        return std::unexpected(PageHeaderError::invalid_page_id);
    }
    if (*read_u32(source, reserved_offset) != 0) {
        return std::unexpected(PageHeaderError::reserved_bytes_nonzero);
    }

    return PageHeader{
        type,
        page_id,
        *read_u64(source, lsn_offset),
        *read_u32(source, page_checksum_offset),
    };
}

}  // namespace nessodb::storage
