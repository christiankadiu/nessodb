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

std::optional<std::size_t> choose_internal_split(
    const std::vector<InternalEntry>& entries) noexcept {
    if (entries.size() < 3) {
        return std::nullopt;
    }

    std::optional<std::size_t> best_split;
    std::size_t best_difference = std::numeric_limits<std::size_t>::max();
    const std::span<const InternalEntry> all_entries{entries};
    for (std::size_t split = 1; split + 1 < entries.size(); ++split) {
        const auto left_size = internal_entries_size(all_entries.first(split));
        const auto right_size =
            internal_entries_size(all_entries.subspan(split + 1));
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

using PageUpdate = std::pair<common::PageId, storage::PageBuffer>;

std::expected<void, BTreeError> add_page_update(
    std::vector<PageUpdate>& updates, const LeafPage& page) {
    storage::PageBuffer buffer{};
    auto encoded = encode_leaf_page(buffer, page);
    if (!encoded) {
        return std::unexpected(BTreeError{encoded.error()});
    }
    updates.emplace_back(page.page_id, std::move(buffer));
    return {};
}

std::expected<void, BTreeError> add_page_update(
    std::vector<PageUpdate>& updates, const InternalPage& page) {
    storage::PageBuffer buffer{};
    auto encoded = encode_internal_page(buffer, page);
    if (!encoded) {
        return std::unexpected(BTreeError{encoded.error()});
    }
    updates.emplace_back(page.page_id, std::move(buffer));
    return {};
}

std::expected<void, BTreeError> write_page_updates(
    storage::BufferPool& buffer_pool, std::vector<PageUpdate> updates) {
    for (auto& [page_id, page] : updates) {
        auto writable = buffer_pool.fetch_index_page_for_write(page_id);
        if (!writable) {
            return std::unexpected(BTreeError{writable.error()});
        }
        **writable = std::move(page);
    }
    return {};
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
    std::vector<common::PageId> internal_path;
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
            const std::optional<common::PageId> parent_page_id =
                internal_path.empty()
                    ? std::nullopt
                    : std::optional{internal_path.back()};
            if (leaf->parent_page_id != parent_page_id ||
                (expected_level && *expected_level != 0)) {
                return std::unexpected(
                    BTreeError{BTreeErrorCode::invalid_tree_structure});
            }
            return LeafLocation{current_page_id, std::move(internal_path)};
        }

        auto internal = decode_internal_page(**page);
        if (!internal) {
            return std::unexpected(BTreeError{internal.error()});
        }
        const std::optional<common::PageId> parent_page_id =
            internal_path.empty()
                ? std::nullopt
                : std::optional{internal_path.back()};
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
        internal_path.push_back(current_page_id);
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
    const std::optional<common::PageId> parent_page_id =
        location->internal_path.empty()
            ? std::nullopt
            : std::optional{location->internal_path.back()};
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
        if ((first_leaf && leaf->parent_page_id != parent_page_id) ||
            (!first_leaf && parent_page_id && !leaf->parent_page_id) ||
            (!first_leaf && !parent_page_id)) {
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

    const std::optional<common::PageId> parent_page_id =
        location->internal_path.empty()
            ? std::nullopt
            : std::optional{location->internal_path.back()};
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
        if (decoded->parent_page_id != parent_page_id) {
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
    if (parent_page_id) {
        return split_child_leaf(std::move(leaf), location->internal_path);
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

    std::vector<PageUpdate> updates;
    if (auto added = add_page_update(updates, right_leaf); !added) {
        return std::unexpected(added.error());
    }
    if (auto added = add_page_update(updates, leaf); !added) {
        return std::unexpected(added.error());
    }
    if (auto added = add_page_update(updates, new_root); !added) {
        return std::unexpected(added.error());
    }
    auto written = write_page_updates(buffer_pool_, std::move(updates));
    if (!written) {
        return std::unexpected(written.error());
    }
    root_page_id_ = new_root_page_id;
    return {};
}

std::expected<void, BTreeError> BTree::split_child_leaf(
    LeafPage leaf, std::span<const common::PageId> internal_path) {
    if (leaf.page_id == root_page_id_ || internal_path.empty() ||
        leaf.parent_page_id != internal_path.back()) {
        return std::unexpected(
            BTreeError{BTreeErrorCode::invalid_tree_structure});
    }
    const common::PageId parent_page_id = internal_path.back();
    const std::optional<common::PageId> grandparent_page_id =
        internal_path.size() == 1
            ? std::nullopt
            : std::optional{internal_path[internal_path.size() - 2]};
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
        if (decoded->parent_page_id != grandparent_page_id ||
            decoded->level != 1) {
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
        if (parent.page_id != root_page_id_) {
            return split_non_root_internal(
                std::move(leaf), std::move(right_entries),
                std::move(parent), insertion_index, internal_path);
        }
        return split_internal_root(std::move(leaf),
                                   std::move(right_entries),
                                   std::move(parent), insertion_index);
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

    std::vector<PageUpdate> updates;
    if (auto added = add_page_update(updates, right_leaf); !added) {
        return std::unexpected(added.error());
    }
    if (auto added = add_page_update(updates, leaf); !added) {
        return std::unexpected(added.error());
    }
    if (auto added = add_page_update(updates, parent); !added) {
        return std::unexpected(added.error());
    }
    return write_page_updates(buffer_pool_, std::move(updates));
}

std::expected<void, BTreeError> BTree::split_non_root_internal(
    LeafPage leaf, std::vector<LeafEntry> right_entries,
    InternalPage parent, std::size_t insertion_index,
    std::span<const common::PageId> internal_path) {
    if (internal_path.size() < 2 ||
        internal_path.front() != root_page_id_ ||
        internal_path.back() != parent.page_id ||
        parent.parent_page_id !=
            internal_path[internal_path.size() - 2] ||
        parent.level != 1 ||
        leaf.parent_page_id != parent.page_id || right_entries.empty() ||
        insertion_index > parent.entries.size()) {
        return std::unexpected(
            BTreeError{BTreeErrorCode::invalid_tree_structure});
    }

    const EncodedKey leaf_separator = right_entries.front().key;
    parent.entries.insert(
        parent.entries.begin() +
            static_cast<std::ptrdiff_t>(insertion_index),
        InternalEntry{leaf_separator, leaf.page_id});
    const auto internal_split = choose_internal_split(parent.entries);
    if (!internal_split) {
        return std::unexpected(BTreeError{IndexPageError::page_full});
    }

    common::PageId right_leaf_page_id;
    {
        auto right_leaf = buffer_pool_.allocate_index_leaf_page();
        if (!right_leaf) {
            return std::unexpected(BTreeError{right_leaf.error()});
        }
        right_leaf_page_id = right_leaf->page_id;
    }
    parent.entries[insertion_index].right_child = right_leaf_page_id;

    const InternalEntry promoted = parent.entries[*internal_split];
    common::PageId right_internal_page_id;
    {
        auto right_internal = buffer_pool_.allocate_index_internal_page(
            parent.level, promoted.right_child);
        if (!right_internal) {
            return std::unexpected(BTreeError{right_internal.error()});
        }
        right_internal_page_id = right_internal->page_id;
    }

    std::vector<InternalEntry> right_internal_entries(
        std::make_move_iterator(
            parent.entries.begin() +
            static_cast<std::ptrdiff_t>(*internal_split + 1)),
        std::make_move_iterator(parent.entries.end()));
    parent.entries.erase(
        parent.entries.begin() +
            static_cast<std::ptrdiff_t>(*internal_split),
        parent.entries.end());
    InternalPage right_internal{
        right_internal_page_id,
        *parent.parent_page_id,
        parent.level,
        promoted.right_child,
        std::move(right_internal_entries),
    };

    InternalPage root;
    {
        const common::PageId root_page_id =
            internal_path[internal_path.size() - 2];
        auto root_page = buffer_pool_.fetch_index_page(root_page_id);
        if (!root_page) {
            return std::unexpected(BTreeError{root_page.error()});
        }
        auto decoded = decode_internal_page(**root_page);
        if (!decoded) {
            return std::unexpected(BTreeError{decoded.error()});
        }
        const std::optional<common::PageId> expected_parent_page_id =
            internal_path.size() == 2
                ? std::nullopt
                : std::optional{
                      internal_path[internal_path.size() - 3]};
        if (decoded->parent_page_id != expected_parent_page_id ||
            decoded->level != 2) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        root = std::move(*decoded);
    }

    std::size_t root_insertion_index = 0;
    if (root.leftmost_child != parent.page_id) {
        const auto child_entry = std::find_if(
            root.entries.begin(), root.entries.end(),
            [&parent](const InternalEntry& entry) {
                return entry.right_child == parent.page_id;
            });
        if (child_entry == root.entries.end()) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        root_insertion_index = static_cast<std::size_t>(
            std::distance(root.entries.begin(), child_entry)) + 1;
    }
    if ((root_insertion_index != 0 &&
         compare_encoded_keys(root.entries[root_insertion_index - 1].key,
                              promoted.key) ==
             std::strong_ordering::greater) ||
        (root_insertion_index != root.entries.size() &&
         compare_encoded_keys(promoted.key,
                              root.entries[root_insertion_index].key) ==
             std::strong_ordering::greater)) {
        return std::unexpected(
            BTreeError{BTreeErrorCode::invalid_tree_structure});
    }
    const auto root_size = internal_entries_size(root.entries, promoted.key);
    if (!root_size || *root_size > storage::page_size) {
        return split_level_two_internal(
            std::move(leaf), std::move(right_entries), right_leaf_page_id,
            std::move(parent), std::move(right_internal), std::move(root),
            root_insertion_index, promoted.key, internal_path);
    }
    root.entries.insert(
        root.entries.begin() +
            static_cast<std::ptrdiff_t>(root_insertion_index),
        InternalEntry{promoted.key, right_internal_page_id});

    std::unordered_set<std::uint64_t> right_internal_children;
    right_internal_children.insert(right_internal.leftmost_child.value);
    for (const auto& entry : right_internal.entries) {
        right_internal_children.insert(entry.right_child.value);
    }
    const auto child_parent = [&right_internal_children,
                               right_internal_page_id,
                               &parent](common::PageId child) {
        return right_internal_children.contains(child.value)
                   ? right_internal_page_id
                   : parent.page_id;
    };

    const auto old_next_page_id = leaf.next_page_id;
    leaf.parent_page_id = child_parent(leaf.page_id);
    leaf.next_page_id = right_leaf_page_id;
    LeafPage right_leaf{
        right_leaf_page_id,
        child_parent(right_leaf_page_id),
        old_next_page_id,
        std::move(right_entries),
    };

    std::vector<std::pair<common::PageId, storage::PageBuffer>>
        reparented_leaf_buffers;
    for (const auto child_value : right_internal_children) {
        const common::PageId child_id{child_value};
        if (child_id == leaf.page_id || child_id == right_leaf_page_id) {
            continue;
        }
        auto child_page = buffer_pool_.fetch_index_page(child_id);
        if (!child_page) {
            return std::unexpected(BTreeError{child_page.error()});
        }
        auto child = decode_leaf_page(**child_page);
        if (!child) {
            return std::unexpected(BTreeError{child.error()});
        }
        if (child->parent_page_id != parent.page_id) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        child->parent_page_id = right_internal_page_id;
        storage::PageBuffer child_buffer{};
        auto encoded_child = encode_leaf_page(child_buffer, *child);
        if (!encoded_child) {
            return std::unexpected(BTreeError{encoded_child.error()});
        }
        reparented_leaf_buffers.emplace_back(child_id,
                                             std::move(child_buffer));
    }

    storage::PageBuffer left_leaf_buffer{};
    storage::PageBuffer right_leaf_buffer{};
    storage::PageBuffer left_internal_buffer{};
    storage::PageBuffer right_internal_buffer{};
    storage::PageBuffer root_buffer{};
    auto encoded_left_leaf = encode_leaf_page(left_leaf_buffer, leaf);
    auto encoded_right_leaf = encode_leaf_page(right_leaf_buffer, right_leaf);
    auto encoded_left_internal =
        encode_internal_page(left_internal_buffer, parent);
    auto encoded_right_internal =
        encode_internal_page(right_internal_buffer, right_internal);
    auto encoded_root = encode_internal_page(root_buffer, root);
    if (!encoded_left_leaf) {
        return std::unexpected(BTreeError{encoded_left_leaf.error()});
    }
    if (!encoded_right_leaf) {
        return std::unexpected(BTreeError{encoded_right_leaf.error()});
    }
    if (!encoded_left_internal) {
        return std::unexpected(BTreeError{encoded_left_internal.error()});
    }
    if (!encoded_right_internal) {
        return std::unexpected(BTreeError{encoded_right_internal.error()});
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
    auto wrote_right_leaf =
        write_page(right_leaf_page_id, std::move(right_leaf_buffer));
    if (!wrote_right_leaf) {
        return std::unexpected(wrote_right_leaf.error());
    }
    auto wrote_left_leaf =
        write_page(leaf.page_id, std::move(left_leaf_buffer));
    if (!wrote_left_leaf) {
        return std::unexpected(wrote_left_leaf.error());
    }
    for (auto& [page_id, page] : reparented_leaf_buffers) {
        auto wrote_child = write_page(page_id, std::move(page));
        if (!wrote_child) {
            return std::unexpected(wrote_child.error());
        }
    }
    auto wrote_right_internal = write_page(
        right_internal_page_id, std::move(right_internal_buffer));
    if (!wrote_right_internal) {
        return std::unexpected(wrote_right_internal.error());
    }
    auto wrote_left_internal =
        write_page(parent.page_id, std::move(left_internal_buffer));
    if (!wrote_left_internal) {
        return std::unexpected(wrote_left_internal.error());
    }
    auto wrote_root = write_page(root.page_id, std::move(root_buffer));
    if (!wrote_root) {
        return std::unexpected(wrote_root.error());
    }
    return {};
}

std::expected<void, BTreeError> BTree::split_level_two_internal(
    LeafPage leaf, std::vector<LeafEntry> right_entries,
    common::PageId right_leaf_page_id, InternalPage left_internal,
    InternalPage right_internal, InternalPage root,
    std::size_t root_insertion_index, EncodedKey separator,
    std::span<const common::PageId> internal_path) {
    if (internal_path.size() < 2) {
        return std::unexpected(
            BTreeError{BTreeErrorCode::invalid_tree_structure});
    }
    const std::optional<common::PageId> expected_root_parent =
        internal_path.size() == 2
            ? std::nullopt
            : std::optional{internal_path[internal_path.size() - 3]};
    if (internal_path[internal_path.size() - 2] != root.page_id ||
        root.parent_page_id != expected_root_parent || root.level != 2 ||
        left_internal.level != 1 ||
        right_internal.level != 1 ||
        left_internal.parent_page_id != root.page_id ||
        right_internal.parent_page_id != root.page_id ||
        leaf.parent_page_id != left_internal.page_id ||
        right_entries.empty() ||
        right_entries.front().key != separator ||
        root_insertion_index > root.entries.size()) {
        return std::unexpected(
            BTreeError{BTreeErrorCode::invalid_tree_structure});
    }

    root.entries.insert(
        root.entries.begin() +
            static_cast<std::ptrdiff_t>(root_insertion_index),
        InternalEntry{separator, right_internal.page_id});
    const auto root_split = choose_internal_split(root.entries);
    if (!root_split) {
        return std::unexpected(BTreeError{IndexPageError::page_full});
    }
    const InternalEntry promoted = root.entries[*root_split];

    common::PageId right_root_page_id;
    {
        auto right_root = buffer_pool_.allocate_index_internal_page(
            root.level, promoted.right_child);
        if (!right_root) {
            return std::unexpected(BTreeError{right_root.error()});
        }
        right_root_page_id = right_root->page_id;
    }

    std::vector<InternalEntry> right_root_entries(
        std::make_move_iterator(
            root.entries.begin() +
            static_cast<std::ptrdiff_t>(*root_split + 1)),
        std::make_move_iterator(root.entries.end()));
    root.entries.erase(
        root.entries.begin() + static_cast<std::ptrdiff_t>(*root_split),
        root.entries.end());
    InternalPage right_root{
        right_root_page_id,
        root.parent_page_id,
        root.level,
        promoted.right_child,
        std::move(right_root_entries),
    };

    std::optional<common::PageId> new_root_page_id;
    InternalPage upper;
    if (!root.parent_page_id) {
        auto new_root = buffer_pool_.allocate_index_internal_page(
            static_cast<std::uint16_t>(root.level + 1), root.page_id);
        if (!new_root) {
            return std::unexpected(BTreeError{new_root.error()});
        }
        new_root_page_id = new_root->page_id;
        root.parent_page_id = *new_root_page_id;
        right_root.parent_page_id = *new_root_page_id;
        upper = InternalPage{
            *new_root_page_id,
            std::nullopt,
            static_cast<std::uint16_t>(root.level + 1),
            root.page_id,
            {{promoted.key, right_root_page_id}},
        };
    } else {
        auto upper_page =
            buffer_pool_.fetch_index_page(*root.parent_page_id);
        if (!upper_page) {
            return std::unexpected(BTreeError{upper_page.error()});
        }
        auto decoded = decode_internal_page(**upper_page);
        if (!decoded) {
            return std::unexpected(BTreeError{decoded.error()});
        }
        const std::optional<common::PageId> expected_upper_parent =
            internal_path.size() == 3
                ? std::nullopt
                : std::optional{
                      internal_path[internal_path.size() - 4]};
        if (decoded->parent_page_id != expected_upper_parent ||
            decoded->level != 3) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        upper = std::move(*decoded);

        std::size_t insertion_index = 0;
        if (upper.leftmost_child != root.page_id) {
            const auto child_entry = std::find_if(
                upper.entries.begin(), upper.entries.end(),
                [&root](const InternalEntry& entry) {
                    return entry.right_child == root.page_id;
                });
            if (child_entry == upper.entries.end()) {
                return std::unexpected(
                    BTreeError{BTreeErrorCode::invalid_tree_structure});
            }
            insertion_index = static_cast<std::size_t>(
                std::distance(upper.entries.begin(), child_entry)) + 1;
        }
        if ((insertion_index != 0 &&
             compare_encoded_keys(upper.entries[insertion_index - 1].key,
                                  promoted.key) ==
                 std::strong_ordering::greater) ||
            (insertion_index != upper.entries.size() &&
             compare_encoded_keys(promoted.key,
                                  upper.entries[insertion_index].key) ==
                 std::strong_ordering::greater)) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        const auto upper_size =
            internal_entries_size(upper.entries, promoted.key);
        if (!upper_size || *upper_size > storage::page_size) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::unsupported_tree_height});
        }
        upper.entries.insert(
            upper.entries.begin() +
                static_cast<std::ptrdiff_t>(insertion_index),
            InternalEntry{promoted.key, right_root_page_id});
    }

    std::unordered_set<std::uint64_t> right_root_children;
    right_root_children.insert(right_root.leftmost_child.value);
    for (const auto& entry : right_root.entries) {
        right_root_children.insert(entry.right_child.value);
    }
    const auto root_child_parent = [&right_root_children,
                                    right_root_page_id,
                                    &root](common::PageId child) {
        return right_root_children.contains(child.value)
                   ? right_root_page_id
                   : root.page_id;
    };
    left_internal.parent_page_id =
        root_child_parent(left_internal.page_id);
    right_internal.parent_page_id =
        root_child_parent(right_internal.page_id);

    std::vector<InternalPage> reparented_internal_pages;
    for (const auto child_value : right_root_children) {
        const common::PageId child_id{child_value};
        if (child_id == left_internal.page_id ||
            child_id == right_internal.page_id) {
            continue;
        }
        auto child_page = buffer_pool_.fetch_index_page(child_id);
        if (!child_page) {
            return std::unexpected(BTreeError{child_page.error()});
        }
        auto child = decode_internal_page(**child_page);
        if (!child) {
            return std::unexpected(BTreeError{child.error()});
        }
        if (child->parent_page_id != root.page_id || child->level != 1) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        child->parent_page_id = right_root_page_id;
        reparented_internal_pages.push_back(std::move(*child));
    }

    std::unordered_set<std::uint64_t> right_internal_children;
    right_internal_children.insert(right_internal.leftmost_child.value);
    for (const auto& entry : right_internal.entries) {
        right_internal_children.insert(entry.right_child.value);
    }
    const auto leaf_parent = [&right_internal_children,
                              &left_internal,
                              &right_internal](common::PageId child) {
        return right_internal_children.contains(child.value)
                   ? right_internal.page_id
                   : left_internal.page_id;
    };

    const auto old_next_page_id = leaf.next_page_id;
    leaf.parent_page_id = leaf_parent(leaf.page_id);
    leaf.next_page_id = right_leaf_page_id;
    LeafPage right_leaf{
        right_leaf_page_id,
        leaf_parent(right_leaf_page_id),
        old_next_page_id,
        std::move(right_entries),
    };

    std::vector<LeafPage> reparented_leaf_pages;
    for (const auto child_value : right_internal_children) {
        const common::PageId child_id{child_value};
        if (child_id == leaf.page_id || child_id == right_leaf_page_id) {
            continue;
        }
        auto child_page = buffer_pool_.fetch_index_page(child_id);
        if (!child_page) {
            return std::unexpected(BTreeError{child_page.error()});
        }
        auto child = decode_leaf_page(**child_page);
        if (!child) {
            return std::unexpected(BTreeError{child.error()});
        }
        if (child->parent_page_id != left_internal.page_id) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        child->parent_page_id = right_internal.page_id;
        reparented_leaf_pages.push_back(std::move(*child));
    }

    std::vector<PageUpdate> updates;
    if (auto added = add_page_update(updates, leaf); !added) {
        return std::unexpected(added.error());
    }
    if (auto added = add_page_update(updates, right_leaf); !added) {
        return std::unexpected(added.error());
    }
    for (const auto& page : reparented_leaf_pages) {
        if (auto added = add_page_update(updates, page); !added) {
            return std::unexpected(added.error());
        }
    }
    if (auto added = add_page_update(updates, left_internal); !added) {
        return std::unexpected(added.error());
    }
    if (auto added = add_page_update(updates, right_internal); !added) {
        return std::unexpected(added.error());
    }
    for (const auto& page : reparented_internal_pages) {
        if (auto added = add_page_update(updates, page); !added) {
            return std::unexpected(added.error());
        }
    }
    if (auto added = add_page_update(updates, root); !added) {
        return std::unexpected(added.error());
    }
    if (auto added = add_page_update(updates, right_root); !added) {
        return std::unexpected(added.error());
    }
    if (auto added = add_page_update(updates, upper); !added) {
        return std::unexpected(added.error());
    }

    auto written = write_page_updates(buffer_pool_, std::move(updates));
    if (!written) {
        return std::unexpected(written.error());
    }
    if (new_root_page_id) {
        root_page_id_ = *new_root_page_id;
    }
    return {};
}

std::expected<void, BTreeError> BTree::split_internal_root(
    LeafPage leaf, std::vector<LeafEntry> right_entries,
    InternalPage root, std::size_t insertion_index) {
    if (root.page_id != root_page_id_ || root.parent_page_id ||
        root.level != 1 || leaf.parent_page_id != root.page_id ||
        right_entries.empty() || insertion_index > root.entries.size()) {
        return std::unexpected(
            BTreeError{BTreeErrorCode::invalid_tree_structure});
    }

    const EncodedKey separator = right_entries.front().key;
    root.entries.insert(
        root.entries.begin() + static_cast<std::ptrdiff_t>(insertion_index),
        InternalEntry{separator, leaf.page_id});
    const auto internal_split = choose_internal_split(root.entries);
    if (!internal_split) {
        return std::unexpected(BTreeError{IndexPageError::page_full});
    }

    common::PageId right_leaf_page_id;
    {
        auto right_leaf = buffer_pool_.allocate_index_leaf_page();
        if (!right_leaf) {
            return std::unexpected(BTreeError{right_leaf.error()});
        }
        right_leaf_page_id = right_leaf->page_id;
    }
    root.entries[insertion_index].right_child = right_leaf_page_id;

    const InternalEntry promoted = root.entries[*internal_split];
    common::PageId right_internal_page_id;
    {
        auto right_internal = buffer_pool_.allocate_index_internal_page(
            root.level, promoted.right_child);
        if (!right_internal) {
            return std::unexpected(BTreeError{right_internal.error()});
        }
        right_internal_page_id = right_internal->page_id;
    }
    common::PageId new_root_page_id;
    {
        auto new_root = buffer_pool_.allocate_index_internal_page(
            static_cast<std::uint16_t>(root.level + 1), root.page_id);
        if (!new_root) {
            return std::unexpected(BTreeError{new_root.error()});
        }
        new_root_page_id = new_root->page_id;
    }

    std::vector<InternalEntry> right_internal_entries(
        std::make_move_iterator(root.entries.begin() +
                                static_cast<std::ptrdiff_t>(*internal_split + 1)),
        std::make_move_iterator(root.entries.end()));
    root.entries.erase(
        root.entries.begin() + static_cast<std::ptrdiff_t>(*internal_split),
        root.entries.end());
    root.parent_page_id = new_root_page_id;
    InternalPage right_internal{
        right_internal_page_id,
        new_root_page_id,
        root.level,
        promoted.right_child,
        std::move(right_internal_entries),
    };
    InternalPage new_root{
        new_root_page_id,
        std::nullopt,
        static_cast<std::uint16_t>(root.level + 1),
        root.page_id,
        {{promoted.key, right_internal_page_id}},
    };

    std::unordered_set<std::uint64_t> right_internal_children;
    right_internal_children.insert(right_internal.leftmost_child.value);
    for (const auto& entry : right_internal.entries) {
        right_internal_children.insert(entry.right_child.value);
    }
    const auto child_parent = [&right_internal_children,
                               right_internal_page_id,
                               &root](common::PageId child) {
        return right_internal_children.contains(child.value)
                   ? right_internal_page_id
                   : root.page_id;
    };

    const auto old_next_page_id = leaf.next_page_id;
    leaf.parent_page_id = child_parent(leaf.page_id);
    leaf.next_page_id = right_leaf_page_id;
    LeafPage right_leaf{
        right_leaf_page_id,
        child_parent(right_leaf_page_id),
        old_next_page_id,
        std::move(right_entries),
    };

    std::vector<std::pair<common::PageId, storage::PageBuffer>>
        reparented_leaf_buffers;
    for (const auto child_value : right_internal_children) {
        const common::PageId child_id{child_value};
        if (child_id == leaf.page_id || child_id == right_leaf_page_id) {
            continue;
        }
        auto child_page = buffer_pool_.fetch_index_page(child_id);
        if (!child_page) {
            return std::unexpected(BTreeError{child_page.error()});
        }
        auto child = decode_leaf_page(**child_page);
        if (!child) {
            return std::unexpected(BTreeError{child.error()});
        }
        if (child->parent_page_id != root.page_id) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        child->parent_page_id = right_internal_page_id;
        storage::PageBuffer child_buffer{};
        auto encoded_child = encode_leaf_page(child_buffer, *child);
        if (!encoded_child) {
            return std::unexpected(BTreeError{encoded_child.error()});
        }
        reparented_leaf_buffers.emplace_back(child_id,
                                             std::move(child_buffer));
    }

    storage::PageBuffer left_leaf_buffer{};
    storage::PageBuffer right_leaf_buffer{};
    storage::PageBuffer left_internal_buffer{};
    storage::PageBuffer right_internal_buffer{};
    storage::PageBuffer root_buffer{};
    auto encoded_left_leaf = encode_leaf_page(left_leaf_buffer, leaf);
    auto encoded_right_leaf = encode_leaf_page(right_leaf_buffer, right_leaf);
    auto encoded_left_internal = encode_internal_page(left_internal_buffer, root);
    auto encoded_right_internal =
        encode_internal_page(right_internal_buffer, right_internal);
    auto encoded_root = encode_internal_page(root_buffer, new_root);
    if (!encoded_left_leaf) {
        return std::unexpected(BTreeError{encoded_left_leaf.error()});
    }
    if (!encoded_right_leaf) {
        return std::unexpected(BTreeError{encoded_right_leaf.error()});
    }
    if (!encoded_left_internal) {
        return std::unexpected(BTreeError{encoded_left_internal.error()});
    }
    if (!encoded_right_internal) {
        return std::unexpected(BTreeError{encoded_right_internal.error()});
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
    auto wrote_right_leaf =
        write_page(right_leaf_page_id, std::move(right_leaf_buffer));
    if (!wrote_right_leaf) {
        return std::unexpected(wrote_right_leaf.error());
    }
    auto wrote_left_leaf = write_page(leaf.page_id, std::move(left_leaf_buffer));
    if (!wrote_left_leaf) {
        return std::unexpected(wrote_left_leaf.error());
    }
    for (auto& [page_id, page] : reparented_leaf_buffers) {
        auto wrote_child = write_page(page_id, std::move(page));
        if (!wrote_child) {
            return std::unexpected(wrote_child.error());
        }
    }
    auto wrote_right_internal = write_page(
        right_internal_page_id, std::move(right_internal_buffer));
    if (!wrote_right_internal) {
        return std::unexpected(wrote_right_internal.error());
    }
    auto wrote_left_internal =
        write_page(root.page_id, std::move(left_internal_buffer));
    if (!wrote_left_internal) {
        return std::unexpected(wrote_left_internal.error());
    }
    auto wrote_root = write_page(new_root_page_id, std::move(root_buffer));
    if (!wrote_root) {
        return std::unexpected(wrote_root.error());
    }
    root_page_id_ = new_root_page_id;
    return {};
}

}  // namespace minidb::index
