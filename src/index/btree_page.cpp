#include "index/btree_page.hpp"

#include "storage/page/codec.hpp"
#include "storage/page/page.hpp"
#include "storage/page/page_header.hpp"

#include <algorithm>
#include <bitset>
#include <limits>
#include <utility>

namespace nessodb::index {
namespace {

constexpr std::size_t version_offset = storage::page_header_size;
constexpr std::size_t entry_count_offset = version_offset + 2;
constexpr std::size_t free_start_offset = entry_count_offset + 2;
constexpr std::size_t free_end_offset = free_start_offset + 2;
constexpr std::size_t parent_page_id_offset = free_end_offset + 2;
constexpr std::size_t special_page_id_offset = parent_page_id_offset + 8;
constexpr std::size_t level_offset = special_page_id_offset + 8;
constexpr std::size_t reserved16_offset = level_offset + 2;
constexpr std::size_t reserved32_offset = reserved16_offset + 2;
constexpr std::size_t leaf_record_id_size = 10;
constexpr std::size_t internal_child_size = 8;

static_assert(reserved32_offset + 4 == index_page_header_size);
static_assert(storage::page_size <=
              std::numeric_limits<std::uint16_t>::max());

struct PageMetadata {
    storage::PageHeader header;
    std::uint16_t entry_count;
    std::uint16_t free_end;
    std::optional<common::PageId> parent_page_id;
    std::uint64_t special_page_id;
    std::uint16_t level;
};

bool valid_non_header_page_id(common::PageId page_id) noexcept {
    return page_id.is_valid() && page_id.value != 0;
}

bool valid_optional_link(std::optional<common::PageId> page_id,
                         common::PageId self) noexcept {
    return !page_id ||
           (valid_non_header_page_id(*page_id) && *page_id != self);
}

std::expected<void, IndexPageError> validate_key(
    std::span<const std::byte> key) {
    const auto values = decode_key(key);
    if (!values || values->empty()) {
        return std::unexpected(IndexPageError::invalid_key);
    }
    return {};
}

bool record_id_less(storage::RecordId left,
                    storage::RecordId right) noexcept {
    if (left.page_id.value != right.page_id.value) {
        return left.page_id.value < right.page_id.value;
    }
    return left.slot_id.value < right.slot_id.value;
}

std::expected<void, IndexPageError> validate_leaf_entries(
    const std::vector<LeafEntry>& entries) {
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        if (!entry.record_id.is_valid()) {
            return std::unexpected(IndexPageError::invalid_entry);
        }
        auto valid_key = validate_key(entry.key);
        if (!valid_key) {
            return std::unexpected(valid_key.error());
        }
        if (index == 0) {
            continue;
        }
        const auto& previous = entries[index - 1];
        const auto comparison = compare_encoded_keys(previous.key, entry.key);
        if (comparison == std::strong_ordering::greater ||
            (comparison == std::strong_ordering::equal &&
             !record_id_less(previous.record_id, entry.record_id))) {
            return std::unexpected(IndexPageError::entries_not_sorted);
        }
    }
    return {};
}

std::expected<void, IndexPageError> validate_internal_entries(
    common::PageId self, common::PageId leftmost_child,
    const std::vector<InternalEntry>& entries) {
    if (!valid_non_header_page_id(leftmost_child) || leftmost_child == self) {
        return std::unexpected(IndexPageError::invalid_link_page_id);
    }
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        if (!valid_non_header_page_id(entry.right_child) ||
            entry.right_child == self) {
            return std::unexpected(IndexPageError::invalid_link_page_id);
        }
        auto valid_key = validate_key(entry.key);
        if (!valid_key) {
            return std::unexpected(valid_key.error());
        }
        if (index != 0 &&
            compare_encoded_keys(entries[index - 1].key, entry.key) ==
                std::strong_ordering::greater) {
            return std::unexpected(IndexPageError::entries_not_sorted);
        }
        if (entry.right_child == leftmost_child) {
            return std::unexpected(IndexPageError::invalid_link_page_id);
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (entries[previous].right_child == entry.right_child) {
                return std::unexpected(IndexPageError::invalid_link_page_id);
            }
        }
    }
    return {};
}

std::expected<std::uint16_t, IndexPageError> encoded_size(
    std::size_t entry_count, std::size_t payload_size) noexcept {
    if (entry_count > std::numeric_limits<std::uint16_t>::max() ||
        entry_count >
            (storage::page_size - index_page_header_size) / index_slot_size) {
        return std::unexpected(IndexPageError::page_full);
    }
    const std::size_t directory_end =
        index_page_header_size + entry_count * index_slot_size;
    if (payload_size > storage::page_size - directory_end) {
        return std::unexpected(IndexPageError::page_full);
    }
    return static_cast<std::uint16_t>(storage::page_size - payload_size);
}

std::expected<void, IndexPageError> encode_metadata(
    std::span<std::byte> destination, storage::PageType type,
    common::PageId page_id, std::optional<common::PageId> parent_page_id,
    std::uint64_t special_page_id, std::uint16_t level,
    std::size_t entry_count, std::uint16_t free_end) noexcept {
    const storage::PageHeader header{type, page_id};
    if (!storage::encode_page_header(destination, header)) {
        return std::unexpected(IndexPageError::invalid_page_header);
    }
    const auto free_start = static_cast<std::uint16_t>(
        index_page_header_size + entry_count * index_slot_size);
    const bool encoded =
        storage::write_u16(destination, version_offset,
                           index_page_format_version) &&
        storage::write_u16(destination, entry_count_offset,
                           static_cast<std::uint16_t>(entry_count)) &&
        storage::write_u16(destination, free_start_offset, free_start) &&
        storage::write_u16(destination, free_end_offset, free_end) &&
        storage::write_u64(destination, parent_page_id_offset,
                           parent_page_id ? parent_page_id->value : 0) &&
        storage::write_u64(destination, special_page_id_offset,
                           special_page_id) &&
        storage::write_u16(destination, level_offset, level) &&
        storage::write_u16(destination, reserved16_offset, 0) &&
        storage::write_u32(destination, reserved32_offset, 0);
    if (!encoded) {
        return std::unexpected(IndexPageError::invalid_page_size);
    }
    return {};
}

std::expected<PageMetadata, IndexPageError> decode_metadata(
    std::span<const std::byte> source, storage::PageType expected_type) {
    if (source.size() != storage::page_size) {
        return std::unexpected(IndexPageError::invalid_page_size);
    }
    auto header = storage::decode_page_header(source);
    if (!header) {
        return std::unexpected(IndexPageError::invalid_page_header);
    }
    if (header->type != expected_type) {
        return std::unexpected(IndexPageError::wrong_page_type);
    }
    if (*storage::read_u16(source, version_offset) !=
        index_page_format_version) {
        return std::unexpected(IndexPageError::unsupported_version);
    }
    if (*storage::read_u16(source, reserved16_offset) != 0 ||
        *storage::read_u32(source, reserved32_offset) != 0) {
        return std::unexpected(IndexPageError::reserved_bytes_nonzero);
    }

    const auto page_id = header->page_id;
    if (!valid_non_header_page_id(page_id)) {
        return std::unexpected(IndexPageError::invalid_page_id);
    }
    const std::uint64_t parent_value =
        *storage::read_u64(source, parent_page_id_offset);
    const std::optional<common::PageId> parent =
        parent_value == 0
            ? std::nullopt
            : std::optional<common::PageId>{common::PageId{parent_value}};
    if (!valid_optional_link(parent, page_id)) {
        return std::unexpected(IndexPageError::invalid_parent_page_id);
    }

    const std::uint16_t count =
        *storage::read_u16(source, entry_count_offset);
    const std::size_t expected_free_start =
        index_page_header_size +
        static_cast<std::size_t>(count) * index_slot_size;
    const std::uint16_t free_start =
        *storage::read_u16(source, free_start_offset);
    const std::uint16_t free_end =
        *storage::read_u16(source, free_end_offset);
    if (expected_free_start > storage::page_size ||
        free_start != expected_free_start || free_end < free_start ||
        free_end > storage::page_size) {
        return std::unexpected(IndexPageError::corrupted_slot_directory);
    }

    return PageMetadata{
        *header,
        count,
        free_end,
        parent,
        *storage::read_u64(source, special_page_id_offset),
        *storage::read_u16(source, level_offset),
    };
}

std::expected<std::vector<std::pair<std::uint16_t, std::uint16_t>>,
              IndexPageError>
decode_slots(std::span<const std::byte> source,
             const PageMetadata& metadata) {
    std::vector<std::pair<std::uint16_t, std::uint16_t>> slots;
    slots.reserve(metadata.entry_count);
    std::bitset<storage::page_size> occupied;
    std::size_t occupied_size = 0;
    for (std::uint16_t index = 0; index < metadata.entry_count; ++index) {
        const std::size_t slot_offset =
            index_page_header_size +
            static_cast<std::size_t>(index) * index_slot_size;
        const std::uint16_t entry_offset =
            *storage::read_u16(source, slot_offset);
        const std::uint16_t entry_size =
            *storage::read_u16(source, slot_offset + 2);
        const std::size_t entry_end =
            static_cast<std::size_t>(entry_offset) + entry_size;
        if (entry_size == 0 || entry_offset < metadata.free_end ||
            entry_end > storage::page_size) {
            return std::unexpected(
                IndexPageError::corrupted_slot_directory);
        }
        for (std::size_t byte = entry_offset; byte < entry_end; ++byte) {
            if (occupied.test(byte)) {
                return std::unexpected(
                    IndexPageError::corrupted_slot_directory);
            }
            occupied.set(byte);
        }
        occupied_size += entry_size;
        slots.emplace_back(entry_offset, entry_size);
    }
    if (occupied_size != storage::page_size - metadata.free_end) {
        return std::unexpected(IndexPageError::corrupted_slot_directory);
    }
    return slots;
}

}  // namespace

std::expected<void, IndexPageError> encode_leaf_page(
    std::span<std::byte> destination, const LeafPage& page) {
    if (destination.size() != storage::page_size) {
        return std::unexpected(IndexPageError::invalid_page_size);
    }
    if (!valid_non_header_page_id(page.page_id)) {
        return std::unexpected(IndexPageError::invalid_page_id);
    }
    if (!valid_optional_link(page.parent_page_id, page.page_id)) {
        return std::unexpected(IndexPageError::invalid_parent_page_id);
    }
    if (!valid_optional_link(page.next_page_id, page.page_id)) {
        return std::unexpected(IndexPageError::invalid_link_page_id);
    }
    auto valid_entries = validate_leaf_entries(page.entries);
    if (!valid_entries) {
        return std::unexpected(valid_entries.error());
    }

    std::size_t payload_size = 0;
    for (const auto& entry : page.entries) {
        if (entry.key.size() > storage::page_size - leaf_record_id_size) {
            return std::unexpected(IndexPageError::page_full);
        }
        const std::size_t entry_size = entry.key.size() + leaf_record_id_size;
        if (payload_size > storage::page_size - entry_size) {
            return std::unexpected(IndexPageError::page_full);
        }
        payload_size += entry_size;
    }
    auto free_end = encoded_size(page.entries.size(), payload_size);
    if (!free_end) {
        return std::unexpected(free_end.error());
    }

    storage::PageBuffer encoded{};
    auto metadata = encode_metadata(
        encoded, storage::PageType::index_leaf, page.page_id,
        page.parent_page_id, page.next_page_id ? page.next_page_id->value : 0,
        0, page.entries.size(), *free_end);
    if (!metadata) {
        return std::unexpected(metadata.error());
    }

    std::size_t cursor = storage::page_size;
    for (std::size_t index = 0; index < page.entries.size(); ++index) {
        const auto& entry = page.entries[index];
        const std::size_t entry_size = entry.key.size() + leaf_record_id_size;
        cursor -= entry_size;
        std::copy(entry.key.begin(), entry.key.end(), encoded.begin() + cursor);
        const bool wrote_entry =
            storage::write_u64(encoded, cursor + entry.key.size(),
                               entry.record_id.page_id.value) &&
            storage::write_u16(encoded, cursor + entry.key.size() + 8,
                               entry.record_id.slot_id.value);
        const std::size_t slot =
            index_page_header_size + index * index_slot_size;
        const bool wrote_slot =
            storage::write_u16(encoded, slot,
                               static_cast<std::uint16_t>(cursor)) &&
            storage::write_u16(encoded, slot + 2,
                               static_cast<std::uint16_t>(entry_size));
        if (!wrote_entry || !wrote_slot) {
            return std::unexpected(IndexPageError::page_full);
        }
    }
    std::copy(encoded.begin(), encoded.end(), destination.begin());
    return {};
}

std::expected<LeafPage, IndexPageError> decode_leaf_page(
    std::span<const std::byte> source) {
    auto metadata = decode_metadata(source, storage::PageType::index_leaf);
    if (!metadata) {
        return std::unexpected(metadata.error());
    }
    if (metadata->level != 0) {
        return std::unexpected(IndexPageError::invalid_level);
    }
    const std::optional<common::PageId> next_page =
        metadata->special_page_id == 0
            ? std::nullopt
            : std::optional<common::PageId>{
                  common::PageId{metadata->special_page_id}};
    if (!valid_optional_link(next_page, metadata->header.page_id)) {
        return std::unexpected(IndexPageError::invalid_link_page_id);
    }
    auto slots = decode_slots(source, *metadata);
    if (!slots) {
        return std::unexpected(slots.error());
    }

    LeafPage page{metadata->header.page_id, metadata->parent_page_id,
                  next_page, {}};
    page.entries.reserve(metadata->entry_count);
    for (const auto& [offset, size] : *slots) {
        if (size <= leaf_record_id_size) {
            return std::unexpected(IndexPageError::invalid_entry);
        }
        const std::size_t key_size = size - leaf_record_id_size;
        EncodedKey key(source.data() + offset,
                       source.data() + offset + key_size);
        auto valid_key = validate_key(key);
        if (!valid_key) {
            return std::unexpected(valid_key.error());
        }
        const storage::RecordId record_id{
            common::PageId{*storage::read_u64(source, offset + key_size)},
            storage::SlotId{*storage::read_u16(
                source, offset + key_size + sizeof(std::uint64_t))},
        };
        if (!record_id.is_valid()) {
            return std::unexpected(IndexPageError::invalid_entry);
        }
        page.entries.push_back(LeafEntry{std::move(key), record_id});
    }
    auto valid_entries = validate_leaf_entries(page.entries);
    if (!valid_entries) {
        return std::unexpected(valid_entries.error());
    }
    return page;
}

std::expected<void, IndexPageError> encode_internal_page(
    std::span<std::byte> destination, const InternalPage& page) {
    if (destination.size() != storage::page_size) {
        return std::unexpected(IndexPageError::invalid_page_size);
    }
    if (!valid_non_header_page_id(page.page_id)) {
        return std::unexpected(IndexPageError::invalid_page_id);
    }
    if (!valid_optional_link(page.parent_page_id, page.page_id)) {
        return std::unexpected(IndexPageError::invalid_parent_page_id);
    }
    if (page.level == 0) {
        return std::unexpected(IndexPageError::invalid_level);
    }
    auto valid_entries = validate_internal_entries(
        page.page_id, page.leftmost_child, page.entries);
    if (!valid_entries) {
        return std::unexpected(valid_entries.error());
    }

    std::size_t payload_size = 0;
    for (const auto& entry : page.entries) {
        if (entry.key.size() > storage::page_size - internal_child_size) {
            return std::unexpected(IndexPageError::page_full);
        }
        const std::size_t entry_size = entry.key.size() + internal_child_size;
        if (payload_size > storage::page_size - entry_size) {
            return std::unexpected(IndexPageError::page_full);
        }
        payload_size += entry_size;
    }
    auto free_end = encoded_size(page.entries.size(), payload_size);
    if (!free_end) {
        return std::unexpected(free_end.error());
    }

    storage::PageBuffer encoded{};
    auto metadata = encode_metadata(
        encoded, storage::PageType::index_internal, page.page_id,
        page.parent_page_id, page.leftmost_child.value, page.level,
        page.entries.size(), *free_end);
    if (!metadata) {
        return std::unexpected(metadata.error());
    }

    std::size_t cursor = storage::page_size;
    for (std::size_t index = 0; index < page.entries.size(); ++index) {
        const auto& entry = page.entries[index];
        const std::size_t entry_size = entry.key.size() + internal_child_size;
        cursor -= entry_size;
        std::copy(entry.key.begin(), entry.key.end(), encoded.begin() + cursor);
        const bool wrote_entry = storage::write_u64(
            encoded, cursor + entry.key.size(),
            entry.right_child.value).has_value();
        const std::size_t slot =
            index_page_header_size + index * index_slot_size;
        const bool wrote_slot =
            storage::write_u16(encoded, slot,
                               static_cast<std::uint16_t>(cursor)) &&
            storage::write_u16(encoded, slot + 2,
                               static_cast<std::uint16_t>(entry_size));
        if (!wrote_entry || !wrote_slot) {
            return std::unexpected(IndexPageError::page_full);
        }
    }
    std::copy(encoded.begin(), encoded.end(), destination.begin());
    return {};
}

std::expected<InternalPage, IndexPageError> decode_internal_page(
    std::span<const std::byte> source) {
    auto metadata = decode_metadata(source, storage::PageType::index_internal);
    if (!metadata) {
        return std::unexpected(metadata.error());
    }
    if (metadata->level == 0) {
        return std::unexpected(IndexPageError::invalid_level);
    }
    const common::PageId leftmost_child{metadata->special_page_id};
    auto slots = decode_slots(source, *metadata);
    if (!slots) {
        return std::unexpected(slots.error());
    }

    InternalPage page{metadata->header.page_id, metadata->parent_page_id,
                      metadata->level, leftmost_child, {}};
    page.entries.reserve(metadata->entry_count);
    for (const auto& [offset, size] : *slots) {
        if (size <= internal_child_size) {
            return std::unexpected(IndexPageError::invalid_entry);
        }
        const std::size_t key_size = size - internal_child_size;
        EncodedKey key(source.data() + offset,
                       source.data() + offset + key_size);
        auto valid_key = validate_key(key);
        if (!valid_key) {
            return std::unexpected(valid_key.error());
        }
        const common::PageId right_child{
            *storage::read_u64(source, offset + key_size)};
        page.entries.push_back(
            InternalEntry{std::move(key), right_child});
    }
    auto valid_entries = validate_internal_entries(
        page.page_id, page.leftmost_child, page.entries);
    if (!valid_entries) {
        return std::unexpected(valid_entries.error());
    }
    return page;
}

}  // namespace nessodb::index
