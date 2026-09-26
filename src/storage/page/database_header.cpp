#include "storage/page/database_header.hpp"

#include "storage/page/codec.hpp"
#include "storage/page/page.hpp"
#include "storage/page/page_header.hpp"

#include <algorithm>

namespace nessodb::storage {
namespace {

inline constexpr std::size_t page_size_offset = page_header_size;
inline constexpr std::size_t database_reserved_offset = page_header_size + 4;
inline constexpr std::size_t page_count_offset = page_header_size + 8;
inline constexpr std::size_t catalog_root_offset = page_header_size + 16;
inline constexpr std::size_t free_list_head_offset = page_header_size + 24;

std::uint64_t encode_reference(const std::optional<common::PageId>& reference) noexcept {
    return reference ? reference->value : 0;
}

std::optional<common::PageId> decode_reference(std::uint64_t value) noexcept {
    if (value == 0) {
        return std::nullopt;
    }
    return common::PageId{value};
}

bool reference_is_valid(const std::optional<common::PageId>& reference,
                        std::uint64_t page_count) noexcept {
    return !reference || (reference->is_valid() && reference->value != 0 &&
                          reference->value < page_count);
}

}  // namespace

std::expected<void, DatabaseHeaderError> encode_database_header(
    std::span<std::byte> destination, const DatabaseHeader& header) noexcept {
    if (destination.size() != page_size) {
        return std::unexpected(DatabaseHeaderError::invalid_page_size);
    }
    if (header.page_count == 0) {
        return std::unexpected(DatabaseHeaderError::invalid_page_count);
    }
    if (!reference_is_valid(header.catalog_root, header.page_count) ||
        !reference_is_valid(header.free_list_head, header.page_count)) {
        return std::unexpected(DatabaseHeaderError::invalid_page_reference);
    }

    std::fill(destination.begin(), destination.end(), std::byte{0});
    if (!encode_page_header(destination,
                            PageHeader{PageType::database_header, common::PageId{0}})) {
        return std::unexpected(DatabaseHeaderError::invalid_page_header);
    }

    const bool encoded = write_u32(destination, page_size_offset,
                                   static_cast<std::uint32_t>(page_size)) &&
                         write_u32(destination, database_reserved_offset, 0) &&
                         write_u64(destination, page_count_offset, header.page_count) &&
                         write_u64(destination, catalog_root_offset,
                                   encode_reference(header.catalog_root)) &&
                         write_u64(destination, free_list_head_offset,
                                   encode_reference(header.free_list_head));
    if (!encoded) {
        return std::unexpected(DatabaseHeaderError::invalid_page_size);
    }
    return {};
}

std::expected<DatabaseHeader, DatabaseHeaderError> decode_database_header(
    std::span<const std::byte> source) noexcept {
    if (source.size() != page_size) {
        return std::unexpected(DatabaseHeaderError::invalid_page_size);
    }

    auto page_header = decode_page_header(source);
    if (!page_header || page_header->type != PageType::database_header) {
        return std::unexpected(DatabaseHeaderError::invalid_page_header);
    }
    if (*read_u32(source, page_size_offset) != page_size) {
        return std::unexpected(DatabaseHeaderError::unexpected_page_size);
    }
    const bool payload_reserved_is_zero =
        std::all_of(source.begin() + database_header_size, source.end(),
                    [](std::byte byte) { return byte == std::byte{0}; });
    if (*read_u32(source, database_reserved_offset) != 0 ||
        !payload_reserved_is_zero) {
        return std::unexpected(DatabaseHeaderError::reserved_bytes_nonzero);
    }

    DatabaseHeader header{
        *read_u64(source, page_count_offset),
        decode_reference(*read_u64(source, catalog_root_offset)),
        decode_reference(*read_u64(source, free_list_head_offset)),
    };
    if (header.page_count == 0) {
        return std::unexpected(DatabaseHeaderError::invalid_page_count);
    }
    if (!reference_is_valid(header.catalog_root, header.page_count) ||
        !reference_is_valid(header.free_list_head, header.page_count)) {
        return std::unexpected(DatabaseHeaderError::invalid_page_reference);
    }
    return header;
}

}  // namespace nessodb::storage
