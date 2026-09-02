#include "index/btree.hpp"

#include "index/key_codec.hpp"
#include "storage/page/page.hpp"

#include <algorithm>
#include <compare>
#include <limits>
#include <optional>
#include <unordered_set>
#include <utility>

namespace minidb::index {
namespace {

std::expected<void, IndexPageError> validate_key(
    std::span<const std::byte> key) {
    const auto values = decode_key(key);
    if (!values || values->empty()) {
        return std::unexpected(IndexPageError::invalid_key);
    }
    return {};
}

std::strong_ordering compare_record_ids(storage::RecordId left,
                                        storage::RecordId right) noexcept {
    if (const auto page_order = left.page_id.value <=> right.page_id.value;
        page_order != 0) {
        return page_order;
    }
    return left.slot_id.value <=> right.slot_id.value;
}

bool entry_less(const LeafEntry& left, const LeafEntry& right) noexcept {
    if (const auto key_order = compare_encoded_keys(left.key, right.key);
        key_order != 0) {
        return key_order == std::strong_ordering::less;
    }
    return compare_record_ids(left.record_id, right.record_id) ==
           std::strong_ordering::less;
}

void sort_record_ids(std::vector<storage::RecordId>& record_ids) {
    std::sort(record_ids.begin(), record_ids.end(),
              [](storage::RecordId left, storage::RecordId right) {
                  return compare_record_ids(left, right) ==
                         std::strong_ordering::less;
              });
}

std::optional<std::size_t> leaf_entries_size(
    std::span<const LeafEntry> entries) noexcept {
    constexpr std::size_t leaf_record_id_size = 10;
    constexpr std::size_t per_entry_overhead =
        leaf_record_id_size + index_slot_size;
    std::size_t size = index_page_header_size;
    for (const auto& entry : entries) {
        if (entry.key.size() >
            std::numeric_limits<std::size_t>::max() - per_entry_overhead) {
            return std::nullopt;
        }
        const std::size_t entry_size =
            entry.key.size() + per_entry_overhead;
        if (size > std::numeric_limits<std::size_t>::max() - entry_size) {
            return std::nullopt;
        }
        size += entry_size;
    }
    return size;
}

std::optional<std::size_t> internal_entries_size(
    std::span<const InternalEntry> entries,
    std::span<const std::byte> additional_key = {}) noexcept {
    constexpr std::size_t internal_child_size = 8;
    constexpr std::size_t per_entry_overhead =
        internal_child_size + index_slot_size;
    std::size_t size = index_page_header_size;
    const auto add_key = [&size](std::size_t key_size) {
        if (key_size >
            std::numeric_limits<std::size_t>::max() - per_entry_overhead) {
            return false;
        }
        const std::size_t entry_size = key_size + per_entry_overhead;
        if (size > std::numeric_limits<std::size_t>::max() - entry_size) {
            return false;
        }
        size += entry_size;
        return true;
    };
    for (const auto& entry : entries) {
        if (!add_key(entry.key.size())) {
            return std::nullopt;
        }
    }
    if (!additional_key.empty() && !add_key(additional_key.size())) {
        return std::nullopt;
    }
    return size;
}

std::optional<std::size_t> choose_leaf_split(
    const std::vector<LeafEntry>& entries) noexcept {
    if (entries.size() < 2) {
        return std::nullopt;
    }

    std::optional<std::size_t> best_split;
    std::size_t best_difference = std::numeric_limits<std::size_t>::max();
    const std::span<const LeafEntry> all_entries{entries};
    for (std::size_t split = 1; split < entries.size(); ++split) {
        const auto left_size = leaf_entries_size(all_entries.first(split));
        const auto right_size = leaf_entries_size(all_entries.subspan(split));
        if (!left_size || !right_size || *left_size > storage::page_size ||
            *right_size > storage::page_size) {
            continue;
        }
        const std::size_t difference =
            *left_size > *right_size ? *left_size - *right_size
                                     : *right_size - *left_size;
        if (difference < best_difference) {
            best_split = split;
            best_difference = difference;
        }
    }
    return best_split;
}

}  // namespace

std::expected<BTree, BTreeError> BTree::create(
    storage::BufferPool& buffer_pool) {
    auto root = buffer_pool.allocate_index_leaf_page();
    if (!root) {
        return std::unexpected(BTreeError{root.error()});
    }
    return BTree{buffer_pool, root->page_id};
}

std::expected<BTree, BTreeError> BTree::open(
    storage::BufferPool& buffer_pool, common::PageId root_page_id) {
    if (!root_page_id.is_valid() || root_page_id.value == 0) {
        return std::unexpected(
            BTreeError{BTreeErrorCode::invalid_root_page_id});
    }

    auto root = buffer_pool.fetch_index_page(root_page_id);
    if (!root) {
        return std::unexpected(BTreeError{root.error()});
    }
    const auto header = storage::decode_page_header(**root);
    if (!header) {
        return std::unexpected(
            BTreeError{BTreeErrorCode::invalid_tree_structure});
    }
    if (header->type == storage::PageType::index_leaf) {
        const auto leaf = decode_leaf_page(**root);
        if (!leaf) {
            return std::unexpected(BTreeError{leaf.error()});
        }
        if (leaf->parent_page_id) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_root_page_id});
        }
    } else {
        const auto internal = decode_internal_page(**root);
        if (!internal) {
            return std::unexpected(BTreeError{internal.error()});
        }
        if (internal->parent_page_id) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_root_page_id});
        }
    }
    return BTree{buffer_pool, root_page_id};
}

BTree::BTree(storage::BufferPool& buffer_pool,
             common::PageId root_page_id) noexcept
    : buffer_pool_(buffer_pool), root_page_id_(root_page_id) {}

common::PageId BTree::root_page_id() const noexcept {
    return root_page_id_;
}

std::expected<BTree::LeafLocation, BTreeError> BTree::locate_leaf(
    std::span<const std::byte> key, bool right_bias) const {
    std::unordered_set<std::uint64_t> visited_pages;
    common::PageId current_page_id = root_page_id_;
    std::optional<common::PageId> parent_page_id;
    std::optional<std::uint16_t> expected_level;
    while (true) {
        if (!visited_pages.insert(current_page_id.value).second) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }

        auto page = buffer_pool_.fetch_index_page(current_page_id);
        if (!page) {
            return std::unexpected(BTreeError{page.error()});
        }
        const auto header = storage::decode_page_header(**page);
        if (!header) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        if (header->type == storage::PageType::index_leaf) {
            auto leaf = decode_leaf_page(**page);
            if (!leaf) {
                return std::unexpected(BTreeError{leaf.error()});
            }
            if (leaf->parent_page_id != parent_page_id ||
                (expected_level && *expected_level != 0)) {
                return std::unexpected(
                    BTreeError{BTreeErrorCode::invalid_tree_structure});
            }
            return LeafLocation{current_page_id, parent_page_id};
        }

        auto internal = decode_internal_page(**page);
        if (!internal) {
            return std::unexpected(BTreeError{internal.error()});
        }
        if (internal->parent_page_id != parent_page_id ||
            (expected_level && internal->level != *expected_level)) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }

        common::PageId child = internal->leftmost_child;
        for (const auto& entry : internal->entries) {
            const auto order = compare_encoded_keys(entry.key, key);
            if (order == std::strong_ordering::greater ||
                (!right_bias && order == std::strong_ordering::equal)) {
                break;
            }
            child = entry.right_child;
        }
        parent_page_id = current_page_id;
        expected_level = static_cast<std::uint16_t>(internal->level - 1);
        current_page_id = child;
    }
}

std::expected<std::vector<storage::RecordId>, BTreeError> BTree::find(
    std::span<const std::byte> key) const {
    auto valid_key = validate_key(key);
    if (!valid_key) {
        return std::unexpected(BTreeError{valid_key.error()});
    }

    auto location = locate_leaf(key, false);
    if (!location) {
        return std::unexpected(location.error());
    }

    std::vector<storage::RecordId> matches;
    std::unordered_set<std::uint64_t> visited_pages;
    common::PageId current_page_id = location->page_id;
    bool first_leaf = true;
    while (true) {
        if (!visited_pages.insert(current_page_id.value).second) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::leaf_chain_cycle});
        }

        auto page = buffer_pool_.fetch_index_page(current_page_id);
        if (!page) {
            return std::unexpected(BTreeError{page.error()});
        }
        auto leaf = decode_leaf_page(**page);
        if (!leaf) {
            return std::unexpected(BTreeError{leaf.error()});
        }
        if ((first_leaf &&
             leaf->parent_page_id != location->parent_page_id) ||
            (!first_leaf && location->parent_page_id &&
             !leaf->parent_page_id) ||
            (!first_leaf && !location->parent_page_id)) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        first_leaf = false;

        for (const auto& entry : leaf->entries) {
            const auto order = compare_encoded_keys(entry.key, key);
            if (order == std::strong_ordering::equal) {
                matches.push_back(entry.record_id);
            } else if (order == std::strong_ordering::greater) {
                sort_record_ids(matches);
                return matches;
            }
        }
        if (!leaf->next_page_id) {
            sort_record_ids(matches);
            return matches;
        }
        current_page_id = *leaf->next_page_id;
    }
}

std::expected<void, BTreeError> BTree::insert(
    std::span<const std::byte> key, storage::RecordId record_id) {
    auto valid_key = validate_key(key);
    if (!valid_key) {
        return std::unexpected(BTreeError{valid_key.error()});
    }
    if (!record_id.is_valid()) {
        return std::unexpected(BTreeError{BTreeErrorCode::invalid_record_id});
    }

    auto existing_records = find(key);
    if (!existing_records) {
        return std::unexpected(existing_records.error());
    }
    if (std::find(existing_records->begin(), existing_records->end(),
                  record_id) != existing_records->end()) {
        return std::unexpected(BTreeError{BTreeErrorCode::duplicate_entry});
    }

    auto location = locate_leaf(key, true);
    if (!location) {
        return std::unexpected(location.error());
    }

    LeafPage leaf;
    {
        auto page = buffer_pool_.fetch_index_page(location->page_id);
        if (!page) {
            return std::unexpected(BTreeError{page.error()});
        }
        auto decoded = decode_leaf_page(**page);
        if (!decoded) {
            return std::unexpected(BTreeError{decoded.error()});
        }
        if (decoded->parent_page_id != location->parent_page_id) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        leaf = std::move(*decoded);
    }

    LeafEntry new_entry{EncodedKey{key.begin(), key.end()}, record_id};
    const auto position = std::lower_bound(
        leaf.entries.begin(), leaf.entries.end(), new_entry, entry_less);
    if (position != leaf.entries.end() && *position == new_entry) {
        return std::unexpected(BTreeError{BTreeErrorCode::duplicate_entry});
    }
    leaf.entries.insert(position, std::move(new_entry));

    storage::PageBuffer encoded{};
    auto encoded_leaf = encode_leaf_page(encoded, leaf);
    if (encoded_leaf) {
        auto writable =
            buffer_pool_.fetch_index_page_for_write(location->page_id);
        if (!writable) {
            return std::unexpected(BTreeError{writable.error()});
        }
        **writable = std::move(encoded);
        return {};
    }
    if (encoded_leaf.error() != IndexPageError::page_full) {
        return std::unexpected(BTreeError{encoded_leaf.error()});
    }
    if (location->parent_page_id) {
        return split_child_leaf(std::move(leaf),
                                *location->parent_page_id);
    }
    return split_root_leaf(std::move(leaf));
}

std::expected<void, BTreeError> BTree::split_root_leaf(LeafPage leaf) {
    if (leaf.page_id != root_page_id_ || leaf.parent_page_id ||
        leaf.next_page_id) {
        return std::unexpected(
            BTreeError{BTreeErrorCode::invalid_tree_structure});
    }
    const auto split = choose_leaf_split(leaf.entries);
    if (!split) {
        return std::unexpected(BTreeError{IndexPageError::page_full});
    }

    common::PageId right_page_id;
    {
        auto right = buffer_pool_.allocate_index_leaf_page();
        if (!right) {
            return std::unexpected(BTreeError{right.error()});
        }
        right_page_id = right->page_id;
    }
    common::PageId new_root_page_id;
    {
        auto root = buffer_pool_.allocate_index_internal_page(1, leaf.page_id);
        if (!root) {
            return std::unexpected(BTreeError{root.error()});
        }
        new_root_page_id = root->page_id;
    }

    std::vector<LeafEntry> right_entries(
        std::make_move_iterator(leaf.entries.begin() +
                                static_cast<std::ptrdiff_t>(*split)),
        std::make_move_iterator(leaf.entries.end()));
    leaf.entries.erase(
        leaf.entries.begin() + static_cast<std::ptrdiff_t>(*split),
        leaf.entries.end());
    leaf.parent_page_id = new_root_page_id;
    leaf.next_page_id = right_page_id;
    LeafPage right_leaf{right_page_id, new_root_page_id, std::nullopt,
                        std::move(right_entries)};
    InternalPage new_root{
        new_root_page_id,
        std::nullopt,
        1,
        leaf.page_id,
        {{right_leaf.entries.front().key, right_page_id}},
    };

    storage::PageBuffer left_buffer{};
    storage::PageBuffer right_buffer{};
    storage::PageBuffer root_buffer{};
    auto encoded_left = encode_leaf_page(left_buffer, leaf);
    auto encoded_right = encode_leaf_page(right_buffer, right_leaf);
    auto encoded_root = encode_internal_page(root_buffer, new_root);
    if (!encoded_left) {
        return std::unexpected(BTreeError{encoded_left.error()});
    }
    if (!encoded_right) {
        return std::unexpected(BTreeError{encoded_right.error()});
    }
    if (!encoded_root) {
        return std::unexpected(BTreeError{encoded_root.error()});
    }

    const auto write_page = [this](common::PageId page_id,
                                   storage::PageBuffer page)
        -> std::expected<void, BTreeError> {
        auto writable = buffer_pool_.fetch_index_page_for_write(page_id);
        if (!writable) {
            return std::unexpected(BTreeError{writable.error()});
        }
        **writable = std::move(page);
        return {};
    };
    auto wrote_right = write_page(right_page_id, std::move(right_buffer));
    if (!wrote_right) {
        return std::unexpected(wrote_right.error());
    }
    auto wrote_left = write_page(leaf.page_id, std::move(left_buffer));
    if (!wrote_left) {
        return std::unexpected(wrote_left.error());
    }
    auto wrote_root = write_page(new_root_page_id, std::move(root_buffer));
    if (!wrote_root) {
        return std::unexpected(wrote_root.error());
    }
    root_page_id_ = new_root_page_id;
    return {};
}

std::expected<void, BTreeError> BTree::split_child_leaf(
    LeafPage leaf, common::PageId parent_page_id) {
    if (leaf.page_id == root_page_id_ ||
        leaf.parent_page_id != parent_page_id ||
        parent_page_id != root_page_id_) {
        return std::unexpected(
            BTreeError{BTreeErrorCode::invalid_tree_structure});
    }
    const auto split = choose_leaf_split(leaf.entries);
    if (!split) {
        return std::unexpected(BTreeError{IndexPageError::page_full});
    }

    std::vector<LeafEntry> right_entries(
        std::make_move_iterator(leaf.entries.begin() +
                                static_cast<std::ptrdiff_t>(*split)),
        std::make_move_iterator(leaf.entries.end()));
    leaf.entries.erase(
        leaf.entries.begin() + static_cast<std::ptrdiff_t>(*split),
        leaf.entries.end());
    const EncodedKey separator = right_entries.front().key;

    InternalPage parent;
    {
        auto parent_page = buffer_pool_.fetch_index_page(parent_page_id);
        if (!parent_page) {
            return std::unexpected(BTreeError{parent_page.error()});
        }
        auto decoded = decode_internal_page(**parent_page);
        if (!decoded) {
            return std::unexpected(BTreeError{decoded.error()});
        }
        if (decoded->parent_page_id || decoded->level != 1) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        parent = std::move(*decoded);
    }

    std::size_t insertion_index = 0;
    if (parent.leftmost_child != leaf.page_id) {
        const auto child_entry = std::find_if(
            parent.entries.begin(), parent.entries.end(),
            [&leaf](const InternalEntry& entry) {
                return entry.right_child == leaf.page_id;
            });
        if (child_entry == parent.entries.end()) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        insertion_index = static_cast<std::size_t>(
            std::distance(parent.entries.begin(), child_entry)) + 1;
    }
    if ((insertion_index != 0 &&
         compare_encoded_keys(parent.entries[insertion_index - 1].key,
                              separator) ==
             std::strong_ordering::greater) ||
        (insertion_index != parent.entries.size() &&
         compare_encoded_keys(separator,
                              parent.entries[insertion_index].key) ==
             std::strong_ordering::greater)) {
        return std::unexpected(
            BTreeError{BTreeErrorCode::invalid_tree_structure});
    }
    const auto parent_size = internal_entries_size(parent.entries, separator);
    if (!parent_size || *parent_size > storage::page_size) {
        return std::unexpected(BTreeError{IndexPageError::page_full});
    }

    common::PageId right_page_id;
    {
        auto right = buffer_pool_.allocate_index_leaf_page();
        if (!right) {
            return std::unexpected(BTreeError{right.error()});
        }
        right_page_id = right->page_id;
    }

    const auto old_next_page_id = leaf.next_page_id;
    leaf.next_page_id = right_page_id;
    LeafPage right_leaf{right_page_id, parent_page_id, old_next_page_id,
                        std::move(right_entries)};
    parent.entries.insert(
        parent.entries.begin() +
            static_cast<std::ptrdiff_t>(insertion_index),
        InternalEntry{separator, right_page_id});

    storage::PageBuffer left_buffer{};
    storage::PageBuffer right_buffer{};
    storage::PageBuffer parent_buffer{};
    auto encoded_left = encode_leaf_page(left_buffer, leaf);
    auto encoded_right = encode_leaf_page(right_buffer, right_leaf);
    auto encoded_parent = encode_internal_page(parent_buffer, parent);
    if (!encoded_left) {
        return std::unexpected(BTreeError{encoded_left.error()});
    }
    if (!encoded_right) {
        return std::unexpected(BTreeError{encoded_right.error()});
    }
    if (!encoded_parent) {
        return std::unexpected(BTreeError{encoded_parent.error()});
    }

    const auto write_page = [this](common::PageId page_id,
                                   storage::PageBuffer page)
        -> std::expected<void, BTreeError> {
        auto writable = buffer_pool_.fetch_index_page_for_write(page_id);
        if (!writable) {
            return std::unexpected(BTreeError{writable.error()});
        }
        **writable = std::move(page);
        return {};
    };
    auto wrote_right = write_page(right_page_id, std::move(right_buffer));
    if (!wrote_right) {
        return std::unexpected(wrote_right.error());
    }
    auto wrote_left = write_page(leaf.page_id, std::move(left_buffer));
    if (!wrote_left) {
        return std::unexpected(wrote_left.error());
    }
    auto wrote_parent = write_page(parent_page_id, std::move(parent_buffer));
    if (!wrote_parent) {
        return std::unexpected(wrote_parent.error());
    }
    return {};
}

}  // namespace minidb::index
