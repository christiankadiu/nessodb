#include "storage/page/slotted_page.hpp"

#include "storage/page/codec.hpp"
#include "storage/page/page.hpp"

#include <algorithm>
#include <limits>

namespace minidb::storage {
namespace {

inline constexpr std::size_t slot_count_offset = page_header_size;
inline constexpr std::size_t free_space_end_offset = page_header_size + 2;
inline constexpr std::size_t heap_reserved_offset = page_header_size + 4;

static_assert(page_size <= std::numeric_limits<std::uint16_t>::max());

}  // namespace

std::expected<SlottedPage, SlottedPageError> SlottedPage::initialize(
    std::span<std::byte> page, common::PageId page_id) noexcept {
    if (page.size() != page_size) {
        return std::unexpected(SlottedPageError::invalid_page_size);
    }

    std::fill(page.begin(), page.end(), std::byte{0});
    const PageHeader header{PageType::heap, page_id};
    if (!encode_page_header(page, header)) {
        return std::unexpected(SlottedPageError::invalid_page_header);
    }

    const bool encoded = write_u16(page, slot_count_offset, 0) &&
                         write_u16(page, free_space_end_offset,
                                   static_cast<std::uint16_t>(page_size)) &&
                         write_u32(page, heap_reserved_offset, 0);
    if (!encoded) {
        return std::unexpected(SlottedPageError::invalid_page_size);
    }
    return SlottedPage{page, header};
}

std::expected<SlottedPage, SlottedPageError> SlottedPage::open(
    std::span<std::byte> page) noexcept {
    if (page.size() != page_size) {
        return std::unexpected(SlottedPageError::invalid_page_size);
    }

    auto header = decode_page_header(page);
    if (!header) {
        return std::unexpected(SlottedPageError::invalid_page_header);
    }
    if (header->type != PageType::heap) {
        return std::unexpected(SlottedPageError::not_a_heap_page);
    }

    const std::uint16_t slots = *read_u16(page, slot_count_offset);
    const std::uint16_t free_end = *read_u16(page, free_space_end_offset);
    const std::size_t directory_end = heap_page_header_size + slots * slot_entry_size;
    if (*read_u32(page, heap_reserved_offset) != 0 || directory_end > free_end ||
        free_end > page_size) {
        return std::unexpected(SlottedPageError::corrupted_slot_directory);
    }
    return SlottedPage{page, *header};
}

common::PageId SlottedPage::page_id() const noexcept {
    return header_.page_id;
}

std::uint16_t SlottedPage::slot_count() const noexcept {
    return *read_u16(page_, slot_count_offset);
}

std::size_t SlottedPage::free_space() const noexcept {
    const std::size_t directory_end = heap_page_header_size + slot_count() * slot_entry_size;
    return *read_u16(page_, free_space_end_offset) - directory_end;
}

SlottedPage::SlottedPage(std::span<std::byte> page, PageHeader header) noexcept
    : page_(page), header_(header) {}

}  // namespace minidb::storage
