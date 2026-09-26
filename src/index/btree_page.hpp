#pragma once

#include "common/page_id.hpp"
#include "index/key_codec.hpp"
#include "storage/access/record_id.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace nessodb::index {

inline constexpr std::uint16_t index_page_format_version = 1;
inline constexpr std::size_t index_page_header_size = 64;
inline constexpr std::size_t index_slot_size = 4;

struct LeafEntry {
    EncodedKey key;
    storage::RecordId record_id;

    friend bool operator==(const LeafEntry&, const LeafEntry&) = default;
};

struct InternalEntry {
    EncodedKey key;
    common::PageId right_child;

    friend bool operator==(const InternalEntry&, const InternalEntry&) = default;
};

struct LeafPage {
    common::PageId page_id;
    std::optional<common::PageId> parent_page_id;
    std::optional<common::PageId> next_page_id;
    std::vector<LeafEntry> entries;

    friend bool operator==(const LeafPage&, const LeafPage&) = default;
};

struct InternalPage {
    common::PageId page_id;
    std::optional<common::PageId> parent_page_id;
    std::uint16_t level{};
    common::PageId leftmost_child;
    std::vector<InternalEntry> entries;

    friend bool operator==(const InternalPage&, const InternalPage&) = default;
};

enum class IndexPageError {
    invalid_page_size,
    invalid_page_header,
    wrong_page_type,
    unsupported_version,
    invalid_page_id,
    invalid_parent_page_id,
    invalid_link_page_id,
    invalid_level,
    reserved_bytes_nonzero,
    corrupted_slot_directory,
    invalid_entry,
    invalid_key,
    entries_not_sorted,
    page_full,
};

[[nodiscard]] std::expected<void, IndexPageError> encode_leaf_page(
    std::span<std::byte> destination, const LeafPage& page);
[[nodiscard]] std::expected<LeafPage, IndexPageError> decode_leaf_page(
    std::span<const std::byte> source);
[[nodiscard]] std::expected<void, IndexPageError> encode_internal_page(
    std::span<std::byte> destination, const InternalPage& page);
[[nodiscard]] std::expected<InternalPage, IndexPageError> decode_internal_page(
    std::span<const std::byte> source);

}  // namespace nessodb::index
