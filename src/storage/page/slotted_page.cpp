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

std::size_t slot_offset(std::uint16_t slot_index) noexcept {
    return heap_page_header_size + static_cast<std::size_t>(slot_index) * slot_entry_size;
}

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

    std::size_t expected_record_end = page_size;
    for (std::uint16_t slot = 0; slot < slots; ++slot) {
        const std::size_t entry = slot_offset(slot);
        const std::uint16_t record_offset = *read_u16(page, entry);
        const std::uint16_t record_size = *read_u16(page, entry + 2);
        if (record_size == 0 || record_offset < free_end ||
            static_cast<std::size_t>(record_offset) + record_size != expected_record_end) {
            return std::unexpected(SlottedPageError::corrupted_slot_directory);
        }
        expected_record_end = record_offset;
    }
    if (expected_record_end != free_end) {
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

std::expected<SlotId, SlottedPageError> SlottedPage::insert(
    std::span<const std::byte> record) noexcept {
    if (record.empty()) {
        return std::unexpected(SlottedPageError::empty_record);
    }
    const std::size_t available = free_space();
    if (available < slot_entry_size || record.size() > available - slot_entry_size) {
        return std::unexpected(SlottedPageError::page_full);
    }

    const std::uint16_t slots = slot_count();
    const std::uint16_t old_free_end = *read_u16(page_, free_space_end_offset);
    const auto record_offset =
        static_cast<std::uint16_t>(old_free_end - record.size());
    std::copy(record.begin(), record.end(), page_.begin() + record_offset);

    const std::size_t entry = slot_offset(slots);
    const bool encoded = write_u16(page_, entry, record_offset) &&
                         write_u16(page_, entry + 2,
                                   static_cast<std::uint16_t>(record.size())) &&
                         write_u16(page_, slot_count_offset,
                                   static_cast<std::uint16_t>(slots + 1)) &&
                         write_u16(page_, free_space_end_offset, record_offset);
    if (!encoded) {
        return std::unexpected(SlottedPageError::corrupted_slot_directory);
    }
    return SlotId{slots};
}

std::expected<std::span<const std::byte>, SlottedPageError> SlottedPage::read(
    SlotId slot_id) const noexcept {
    if (!slot_id.is_valid() || slot_id.value >= slot_count()) {
        return std::unexpected(SlottedPageError::invalid_slot);
    }

    const std::size_t entry = slot_offset(slot_id.value);
    const std::uint16_t record_offset = *read_u16(page_, entry);
    const std::uint16_t record_size = *read_u16(page_, entry + 2);
    return std::span<const std::byte>{page_.data() + record_offset, record_size};
}

SlottedPage::SlottedPage(std::span<std::byte> page, PageHeader header) noexcept
    : page_(page), header_(header) {}

}  // namespace minidb::storage
