#include "storage/page/slotted_page.hpp"

#include "storage/page/codec.hpp"
#include "storage/page/page.hpp"

#include <algorithm>
#include <bitset>
#include <limits>

namespace minidb::storage {
namespace {

inline constexpr std::size_t slot_count_offset = page_header_size;
inline constexpr std::size_t free_space_end_offset = page_header_size + 2;
inline constexpr std::size_t heap_version_offset = page_header_size + 4;
inline constexpr std::size_t heap_reserved_offset = page_header_size + 6;
inline constexpr std::size_t next_page_id_offset = page_header_size + 8;

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
                         write_u16(page, heap_version_offset, heap_page_format_version) &&
                         write_u16(page, heap_reserved_offset, 0) &&
                         write_u64(page, next_page_id_offset, 0);
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
    if (*read_u16(page, heap_version_offset) != heap_page_format_version) {
        return std::unexpected(SlottedPageError::unsupported_heap_page_version);
    }

    const auto next_page = *read_u64(page, next_page_id_offset);
    if (next_page == common::PageId::invalid_value || next_page == header->page_id.value) {
        return std::unexpected(SlottedPageError::invalid_next_page_id);
    }

    const std::uint16_t slots = *read_u16(page, slot_count_offset);
    const std::uint16_t free_end = *read_u16(page, free_space_end_offset);
    const std::size_t directory_end = heap_page_header_size + slots * slot_entry_size;
    if (*read_u16(page, heap_reserved_offset) != 0 || directory_end > free_end ||
        free_end > page_size) {
        return std::unexpected(SlottedPageError::corrupted_slot_directory);
    }

    std::bitset<page_size> occupied_bytes;
    std::size_t occupied_size = 0;
    for (std::uint16_t slot = 0; slot < slots; ++slot) {
        const std::size_t entry = slot_offset(slot);
        const std::uint16_t record_offset = *read_u16(page, entry);
        const std::uint16_t record_size = *read_u16(page, entry + 2);
        if (record_size == 0) {
            if (record_offset != 0) {
                return std::unexpected(
                    SlottedPageError::corrupted_slot_directory);
            }
            continue;
        }

        const std::size_t record_end =
            static_cast<std::size_t>(record_offset) + record_size;
        if (record_offset < free_end || record_end > page_size) {
            return std::unexpected(SlottedPageError::corrupted_slot_directory);
        }
        for (std::size_t byte = record_offset; byte < record_end; ++byte) {
            if (occupied_bytes.test(byte)) {
                return std::unexpected(
                    SlottedPageError::corrupted_slot_directory);
            }
            occupied_bytes.set(byte);
        }
        occupied_size += record_size;
    }
    if (occupied_size != page_size - free_end) {
        return std::unexpected(SlottedPageError::corrupted_slot_directory);
    }
    return SlottedPage{page, *header};
}

common::PageId SlottedPage::page_id() const noexcept {
    return header_.page_id;
}

std::optional<common::PageId> SlottedPage::next_page_id() const noexcept {
    const auto value = *read_u64(page_, next_page_id_offset);
    if (value == 0) {
        return std::nullopt;
    }
    return common::PageId{value};
}

std::expected<void, SlottedPageError> SlottedPage::set_next_page_id(
    std::optional<common::PageId> page_id) noexcept {
    if (page_id && (!page_id->is_valid() || page_id->value == 0 || *page_id == header_.page_id)) {
        return std::unexpected(SlottedPageError::invalid_next_page_id);
    }
    if (!write_u64(page_, next_page_id_offset, page_id ? page_id->value : 0)) {
        return std::unexpected(SlottedPageError::invalid_page_size);
    }
    return {};
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

    const std::uint16_t slots = slot_count();
    std::optional<SlotId> reusable_slot;
    for (std::uint16_t slot = 0; slot < slots; ++slot) {
        const std::size_t entry = slot_offset(slot);
        if (*read_u16(page_, entry + 2) == 0) {
            reusable_slot = SlotId{slot};
            break;
        }
    }

    const std::size_t available = free_space();
    const std::size_t directory_growth = reusable_slot ? 0 : slot_entry_size;
    if (available < directory_growth ||
        record.size() > available - directory_growth) {
        return std::unexpected(SlottedPageError::page_full);
    }

    const std::uint16_t old_free_end = *read_u16(page_, free_space_end_offset);
    const auto record_offset =
        static_cast<std::uint16_t>(old_free_end - record.size());
    std::copy(record.begin(), record.end(), page_.begin() + record_offset);

    const SlotId slot_id = reusable_slot.value_or(SlotId{slots});
    const std::size_t entry = slot_offset(slot_id.value);
    const bool encoded = write_u16(page_, entry, record_offset) &&
                         write_u16(page_, entry + 2,
                                   static_cast<std::uint16_t>(record.size())) &&
                         (reusable_slot ||
                          write_u16(page_, slot_count_offset,
                                    static_cast<std::uint16_t>(slots + 1))) &&
                         write_u16(page_, free_space_end_offset, record_offset);
    if (!encoded) {
        return std::unexpected(SlottedPageError::corrupted_slot_directory);
    }
    return slot_id;
}

std::expected<std::span<const std::byte>, SlottedPageError> SlottedPage::read(
    SlotId slot_id) const noexcept {
    if (!slot_id.is_valid() || slot_id.value >= slot_count()) {
        return std::unexpected(SlottedPageError::invalid_slot);
    }

    const std::size_t entry = slot_offset(slot_id.value);
    const std::uint16_t record_offset = *read_u16(page_, entry);
    const std::uint16_t record_size = *read_u16(page_, entry + 2);
    if (record_size == 0) {
        return std::unexpected(SlottedPageError::deleted_slot);
    }
    return std::span<const std::byte>{page_.data() + record_offset, record_size};
}

std::expected<void, SlottedPageError> SlottedPage::erase(
    SlotId slot_id) noexcept {
    if (!slot_id.is_valid() || slot_id.value >= slot_count()) {
        return std::unexpected(SlottedPageError::invalid_slot);
    }

    const std::size_t erased_entry = slot_offset(slot_id.value);
    const std::uint16_t record_offset = *read_u16(page_, erased_entry);
    const std::uint16_t record_size = *read_u16(page_, erased_entry + 2);
    if (record_size == 0) {
        return std::unexpected(SlottedPageError::deleted_slot);
    }

    const std::uint16_t old_free_end =
        *read_u16(page_, free_space_end_offset);
    std::move_backward(page_.begin() + old_free_end,
                       page_.begin() + record_offset,
                       page_.begin() + record_offset + record_size);
    std::fill(page_.begin() + old_free_end,
              page_.begin() + old_free_end + record_size, std::byte{0});

    for (std::uint16_t slot = 0; slot < slot_count(); ++slot) {
        if (slot == slot_id.value) {
            continue;
        }
        const std::size_t entry = slot_offset(slot);
        const std::uint16_t offset = *read_u16(page_, entry);
        const std::uint16_t size = *read_u16(page_, entry + 2);
        if (size != 0 && offset < record_offset &&
            !write_u16(page_, entry,
                       static_cast<std::uint16_t>(offset + record_size))) {
            return std::unexpected(
                SlottedPageError::corrupted_slot_directory);
        }
    }

    const bool encoded = write_u16(page_, erased_entry, 0) &&
                         write_u16(page_, erased_entry + 2, 0) &&
                         write_u16(page_, free_space_end_offset,
                                   static_cast<std::uint16_t>(old_free_end +
                                                              record_size));
    if (!encoded) {
        return std::unexpected(SlottedPageError::corrupted_slot_directory);
    }
    return {};
}

SlottedPage::SlottedPage(std::span<std::byte> page, PageHeader header) noexcept
    : page_(page), header_(header) {}

}  // namespace minidb::storage
