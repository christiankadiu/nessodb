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

std::vector<common::PageId> internal_children(
    const InternalPage& page) {
    std::vector<common::PageId> children;
    children.reserve(page.entries.size() + 1);
    children.push_back(page.leftmost_child);
    for (const auto& entry : page.entries) {
        children.push_back(entry.right_child);
    }
    return children;
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

BTreeCursor::BTreeCursor(storage::BufferPool& buffer_pool, LeafPage leaf,
                         std::size_t entry_index,
                         std::optional<BTreeKeyBound> lower_bound,
                         std::optional<BTreeKeyBound> upper_bound)
    : buffer_pool_(&buffer_pool),
      leaf_(std::move(leaf)),
      entry_index_(entry_index),
      lower_bound_(std::move(lower_bound)),
      upper_bound_(std::move(upper_bound)),
      requires_parent_(leaf_->parent_page_id.has_value()) {
    visited_pages_.insert(leaf_->page_id.value);
}

std::expected<std::optional<LeafEntry>, BTreeError> BTreeCursor::next() {
    while (leaf_) {
        if (entry_index_ < leaf_->entries.size()) {
            const LeafEntry& entry = leaf_->entries[entry_index_++];
            if (last_key_ &&
                compare_encoded_keys(entry.key, *last_key_) ==
                    std::strong_ordering::less) {
                return std::unexpected(
                    BTreeError{BTreeErrorCode::invalid_tree_structure});
            }
            if (lower_bound_) {
                const auto order =
                    compare_encoded_keys(entry.key, lower_bound_->key);
                if (order == std::strong_ordering::less ||
                    (order == std::strong_ordering::equal &&
                     !lower_bound_->inclusive)) {
                    last_key_ = entry.key;
                    continue;
                }
                lower_bound_.reset();
            }
            if (upper_bound_) {
                const auto order =
                    compare_encoded_keys(entry.key, upper_bound_->key);
                if (order == std::strong_ordering::greater ||
                    (order == std::strong_ordering::equal &&
                     !upper_bound_->inclusive)) {
                    leaf_.reset();
                    return std::optional<LeafEntry>{};
                }
            }
            last_key_ = entry.key;
            return std::optional<LeafEntry>{entry};
        }

        if (!leaf_->next_page_id) {
            leaf_.reset();
            return std::optional<LeafEntry>{};
        }
        if (!requires_parent_) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }

        const common::PageId next_page_id = *leaf_->next_page_id;
        if (!visited_pages_.insert(next_page_id.value).second) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::leaf_chain_cycle});
        }
        auto page = buffer_pool_->fetch_index_page(next_page_id);
        if (!page) {
            return std::unexpected(BTreeError{page.error()});
        }
        auto decoded = decode_leaf_page(**page);
        if (!decoded) {
            return std::unexpected(BTreeError{decoded.error()});
        }
        if (!decoded->parent_page_id) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        leaf_ = std::move(*decoded);
        entry_index_ = 0;
    }
    return std::optional<LeafEntry>{};
}

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

std::expected<BTree::LeafLocation, BTreeError>
BTree::locate_leftmost_leaf() const {
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
        const std::optional<common::PageId> parent_page_id =
            internal_path.empty()
                ? std::nullopt
                : std::optional{internal_path.back()};
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
            return LeafLocation{current_page_id,
                                std::move(internal_path)};
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
        internal_path.push_back(current_page_id);
        expected_level = static_cast<std::uint16_t>(internal->level - 1);
        current_page_id = internal->leftmost_child;
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

std::expected<BTreeCursor, BTreeError> BTree::scan(
    BTreeRange range) const {
    if (range.lower) {
        auto valid = validate_key(range.lower->key);
        if (!valid) {
            return std::unexpected(BTreeError{valid.error()});
        }
    }
    if (range.upper) {
        auto valid = validate_key(range.upper->key);
        if (!valid) {
            return std::unexpected(BTreeError{valid.error()});
        }
    }
    if (range.lower && range.upper &&
        compare_encoded_keys(range.lower->key, range.upper->key) ==
            std::strong_ordering::greater) {
        return std::unexpected(BTreeError{BTreeErrorCode::invalid_range});
    }

    std::expected<LeafLocation, BTreeError> location =
        range.lower ? locate_leaf(range.lower->key, false)
                    : locate_leftmost_leaf();
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
        const std::optional<common::PageId> parent_page_id =
            location->internal_path.empty()
                ? std::nullopt
                : std::optional{location->internal_path.back()};
        if (decoded->parent_page_id != parent_page_id) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        leaf = std::move(*decoded);
    }

    std::size_t entry_index = 0;
    if (range.lower) {
        while (entry_index < leaf.entries.size()) {
            const auto order = compare_encoded_keys(
                leaf.entries[entry_index].key, range.lower->key);
            if (order == std::strong_ordering::greater ||
                (order == std::strong_ordering::equal &&
                 range.lower->inclusive)) {
                break;
            }
            ++entry_index;
        }
    }
    return BTreeCursor{buffer_pool_, std::move(leaf), entry_index,
                       std::move(range.lower), std::move(range.upper)};
}

std::expected<void, BTreeError> BTree::erase(
    std::span<const std::byte> key, storage::RecordId record_id) {
    auto valid_key = validate_key(key);
    if (!valid_key) {
        return std::unexpected(BTreeError{valid_key.error()});
    }
    if (!record_id.is_valid()) {
        return std::unexpected(BTreeError{BTreeErrorCode::invalid_record_id});
    }

    auto location = locate_leaf(key, false);
    if (!location) {
        return std::unexpected(location.error());
    }

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

        LeafPage leaf;
        {
            auto page = buffer_pool_.fetch_index_page(current_page_id);
            if (!page) {
                return std::unexpected(BTreeError{page.error()});
            }
            auto decoded = decode_leaf_page(**page);
            if (!decoded) {
                return std::unexpected(BTreeError{decoded.error()});
            }
            if ((first_leaf && decoded->parent_page_id != parent_page_id) ||
                (!first_leaf && parent_page_id &&
                 !decoded->parent_page_id) ||
                (!first_leaf && !parent_page_id)) {
                return std::unexpected(
                    BTreeError{BTreeErrorCode::invalid_tree_structure});
            }
            leaf = std::move(*decoded);
        }
        first_leaf = false;

        for (auto entry = leaf.entries.begin();
             entry != leaf.entries.end(); ++entry) {
            const auto order = compare_encoded_keys(entry->key, key);
            if (order == std::strong_ordering::greater) {
                return std::unexpected(
                    BTreeError{BTreeErrorCode::entry_not_found});
            }
            if (order != std::strong_ordering::equal ||
                entry->record_id != record_id) {
                continue;
            }

            leaf.entries.erase(entry);
            return rebalance_leaf_after_erase(std::move(leaf));
        }

        if (!leaf.next_page_id) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::entry_not_found});
        }
        current_page_id = *leaf.next_page_id;
    }
}

std::expected<void, BTreeError> BTree::rebalance_leaf_after_erase(
    LeafPage leaf) {
    const auto write_leaf = [this](const LeafPage& page)
        -> std::expected<void, BTreeError> {
        std::vector<PageUpdate> updates;
        if (auto added = add_page_update(updates, page); !added) {
            return std::unexpected(added.error());
        }
        return write_page_updates(buffer_pool_, std::move(updates));
    };

    if (!leaf.parent_page_id) {
        if (leaf.page_id != root_page_id_ || leaf.next_page_id) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        return write_leaf(leaf);
    }

    InternalPage parent;
    {
        auto page = buffer_pool_.fetch_index_page(*leaf.parent_page_id);
        if (!page) {
            return std::unexpected(BTreeError{page.error()});
        }
        auto decoded = decode_internal_page(**page);
        if (!decoded) {
            return std::unexpected(BTreeError{decoded.error()});
        }
        if (decoded->level != 1) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        parent = std::move(*decoded);
    }

    std::size_t child_index = 0;
    if (parent.leftmost_child != leaf.page_id) {
        const auto child = std::find_if(
            parent.entries.begin(), parent.entries.end(),
            [&leaf](const InternalEntry& entry) {
                return entry.right_child == leaf.page_id;
            });
        if (child == parent.entries.end()) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        child_index = static_cast<std::size_t>(
                          std::distance(parent.entries.begin(), child)) +
                      1;
    }

    const auto leaf_size = leaf_entries_size(leaf.entries);
    if (!leaf_size) {
        return std::unexpected(BTreeError{IndexPageError::page_full});
    }
    if (*leaf_size >= storage::page_size / 2) {
        std::vector<PageUpdate> updates;
        if (child_index != 0 && !leaf.entries.empty()) {
            parent.entries[child_index - 1].key = leaf.entries.front().key;
            if (auto added = add_page_update(updates, parent); !added) {
                return std::unexpected(added.error());
            }
        }
        if (auto added = add_page_update(updates, leaf); !added) {
            return std::unexpected(added.error());
        }
        return write_page_updates(buffer_pool_, std::move(updates));
    }

    if (parent.entries.empty()) {
        if (parent.page_id == root_page_id_) {
            leaf.parent_page_id.reset();
            auto written = write_leaf(leaf);
            if (!written) {
                return std::unexpected(written.error());
            }
            root_page_id_ = leaf.page_id;
            return {};
        }

        const common::PageId leaf_page_id = leaf.page_id;
        auto written = write_leaf(leaf);
        if (!written) {
            return std::unexpected(written.error());
        }
        auto internal_rebalanced =
            rebalance_internal_after_erase(std::move(parent));
        if (!internal_rebalanced) {
            return std::unexpected(internal_rebalanced.error());
        }
        auto page = buffer_pool_.fetch_index_page(leaf_page_id);
        if (!page) {
            return std::unexpected(BTreeError{page.error()});
        }
        auto reparented_leaf = decode_leaf_page(**page);
        if (!reparented_leaf) {
            return std::unexpected(BTreeError{reparented_leaf.error()});
        }
        return rebalance_leaf_after_erase(std::move(*reparented_leaf));
    }

    const std::size_t sibling_index =
        child_index == 0 ? 1 : child_index - 1;
    const common::PageId sibling_page_id =
        sibling_index == 0
            ? parent.leftmost_child
            : parent.entries[sibling_index - 1].right_child;
    LeafPage sibling;
    {
        auto page = buffer_pool_.fetch_index_page(sibling_page_id);
        if (!page) {
            return std::unexpected(BTreeError{page.error()});
        }
        auto decoded = decode_leaf_page(**page);
        if (!decoded) {
            return std::unexpected(BTreeError{decoded.error()});
        }
        if (decoded->parent_page_id != parent.page_id) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        sibling = std::move(*decoded);
    }

    LeafPage left = sibling_index < child_index ? std::move(sibling)
                                                : std::move(leaf);
    LeafPage right = sibling_index < child_index ? std::move(leaf)
                                                 : std::move(sibling);
    if (left.next_page_id != right.page_id) {
        return std::unexpected(
            BTreeError{BTreeErrorCode::invalid_tree_structure});
    }

    std::vector<LeafEntry> combined;
    combined.reserve(left.entries.size() + right.entries.size());
    combined.insert(combined.end(),
                    std::make_move_iterator(left.entries.begin()),
                    std::make_move_iterator(left.entries.end()));
    combined.insert(combined.end(),
                    std::make_move_iterator(right.entries.begin()),
                    std::make_move_iterator(right.entries.end()));
    std::sort(combined.begin(), combined.end(), entry_less);

    const std::size_t separator_index =
        sibling_index < child_index ? child_index - 1 : child_index;
    const auto combined_size = leaf_entries_size(combined);
    if (combined_size && *combined_size <= storage::page_size) {
        left.entries = std::move(combined);
        left.next_page_id = right.next_page_id;
        parent.entries.erase(
            parent.entries.begin() +
            static_cast<std::ptrdiff_t>(separator_index));

        std::vector<PageUpdate> updates;
        if (auto added = add_page_update(updates, left); !added) {
            return std::unexpected(added.error());
        }
        auto written = write_page_updates(buffer_pool_, std::move(updates));
        if (!written) {
            return std::unexpected(written.error());
        }
        return rebalance_internal_after_erase(std::move(parent));
    }

    const auto split = choose_leaf_split(combined);
    if (!split) {
        return std::unexpected(BTreeError{IndexPageError::page_full});
    }
    left.entries.assign(
        std::make_move_iterator(combined.begin()),
        std::make_move_iterator(
            combined.begin() + static_cast<std::ptrdiff_t>(*split)));
    right.entries.assign(
        std::make_move_iterator(
            combined.begin() + static_cast<std::ptrdiff_t>(*split)),
        std::make_move_iterator(combined.end()));
    parent.entries[separator_index].key = right.entries.front().key;

    std::vector<PageUpdate> updates;
    if (auto added = add_page_update(updates, left); !added) {
        return std::unexpected(added.error());
    }
    if (auto added = add_page_update(updates, right); !added) {
        return std::unexpected(added.error());
    }
    if (auto added = add_page_update(updates, parent); !added) {
        return std::unexpected(added.error());
    }
    return write_page_updates(buffer_pool_, std::move(updates));
}

std::expected<void, BTreeError> BTree::rebalance_internal_after_erase(
    InternalPage node) {
    const auto write_internal = [this](const InternalPage& page)
        -> std::expected<void, BTreeError> {
        std::vector<PageUpdate> updates;
        if (auto added = add_page_update(updates, page); !added) {
            return std::unexpected(added.error());
        }
        return write_page_updates(buffer_pool_, std::move(updates));
    };

    if (node.page_id == root_page_id_) {
        if (node.parent_page_id) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        if (!node.entries.empty()) {
            return write_internal(node);
        }
        auto reparented = reparent_child(
            node.leftmost_child,
            static_cast<std::uint16_t>(node.level - 1), node.page_id,
            std::nullopt);
        if (!reparented) {
            return std::unexpected(reparented.error());
        }
        root_page_id_ = node.leftmost_child;
        if (node.level > 1) {
            auto page = buffer_pool_.fetch_index_page(root_page_id_);
            if (!page) {
                return std::unexpected(BTreeError{page.error()});
            }
            auto child = decode_internal_page(**page);
            if (!child) {
                return std::unexpected(BTreeError{child.error()});
            }
            return rebalance_internal_after_erase(std::move(*child));
        }
        return {};
    }
    if (!node.parent_page_id) {
        return std::unexpected(
            BTreeError{BTreeErrorCode::invalid_tree_structure});
    }

    const auto node_size = internal_entries_size(node.entries);
    if (!node_size) {
        return std::unexpected(BTreeError{IndexPageError::page_full});
    }
    if (*node_size >= storage::page_size / 2) {
        return write_internal(node);
    }

    InternalPage parent;
    {
        if (node.level == std::numeric_limits<std::uint16_t>::max()) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::unsupported_tree_height});
        }
        auto page = buffer_pool_.fetch_index_page(*node.parent_page_id);
        if (!page) {
            return std::unexpected(BTreeError{page.error()});
        }
        auto decoded = decode_internal_page(**page);
        if (!decoded) {
            return std::unexpected(BTreeError{decoded.error()});
        }
        if (decoded->level != static_cast<std::uint16_t>(node.level + 1)) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        parent = std::move(*decoded);
    }

    std::size_t child_index = 0;
    if (parent.leftmost_child != node.page_id) {
        const auto child = std::find_if(
            parent.entries.begin(), parent.entries.end(),
            [&node](const InternalEntry& entry) {
                return entry.right_child == node.page_id;
            });
        if (child == parent.entries.end()) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        child_index = static_cast<std::size_t>(
                          std::distance(parent.entries.begin(), child)) +
                      1;
    }
    if (parent.entries.empty()) {
        auto written = write_internal(node);
        if (!written) {
            return std::unexpected(written.error());
        }
        return rebalance_internal_after_erase(std::move(parent));
    }

    const std::size_t sibling_index =
        child_index == 0 ? 1 : child_index - 1;
    const common::PageId sibling_page_id =
        sibling_index == 0
            ? parent.leftmost_child
            : parent.entries[sibling_index - 1].right_child;
    InternalPage sibling;
    {
        auto page = buffer_pool_.fetch_index_page(sibling_page_id);
        if (!page) {
            return std::unexpected(BTreeError{page.error()});
        }
        auto decoded = decode_internal_page(**page);
        if (!decoded) {
            return std::unexpected(BTreeError{decoded.error()});
        }
        if (decoded->parent_page_id != parent.page_id ||
            decoded->level != node.level) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        sibling = std::move(*decoded);
    }

    InternalPage left = sibling_index < child_index ? std::move(sibling)
                                                    : std::move(node);
    InternalPage right = sibling_index < child_index ? std::move(node)
                                                     : std::move(sibling);
    const std::size_t separator_index =
        sibling_index < child_index ? child_index - 1 : child_index;
    const auto left_children_before = internal_children(left);
    const auto right_children_before = internal_children(right);

    std::vector<InternalEntry> combined;
    combined.reserve(left.entries.size() + right.entries.size() + 1);
    combined.insert(combined.end(),
                    std::make_move_iterator(left.entries.begin()),
                    std::make_move_iterator(left.entries.end()));
    combined.push_back(InternalEntry{
        parent.entries[separator_index].key, right.leftmost_child});
    combined.insert(combined.end(),
                    std::make_move_iterator(right.entries.begin()),
                    std::make_move_iterator(right.entries.end()));

    const auto combined_size = internal_entries_size(combined);
    if (combined_size && *combined_size <= storage::page_size) {
        left.entries = std::move(combined);
        parent.entries.erase(
            parent.entries.begin() +
            static_cast<std::ptrdiff_t>(separator_index));
        for (const auto child_page_id : right_children_before) {
            auto reparented = reparent_child(
                child_page_id,
                static_cast<std::uint16_t>(right.level - 1),
                right.page_id, left.page_id);
            if (!reparented) {
                return std::unexpected(reparented.error());
            }
        }
        auto written = write_internal(left);
        if (!written) {
            return std::unexpected(written.error());
        }
        return rebalance_internal_after_erase(std::move(parent));
    }

    const auto split = choose_internal_split(combined);
    if (!split) {
        return std::unexpected(BTreeError{IndexPageError::page_full});
    }
    const InternalEntry promoted = combined[*split];
    left.entries.assign(
        std::make_move_iterator(combined.begin()),
        std::make_move_iterator(
            combined.begin() + static_cast<std::ptrdiff_t>(*split)));
    right.leftmost_child = promoted.right_child;
    right.entries.assign(
        std::make_move_iterator(
            combined.begin() + static_cast<std::ptrdiff_t>(*split + 1)),
        std::make_move_iterator(combined.end()));
    parent.entries[separator_index].key = promoted.key;

    for (const auto child_page_id : internal_children(left)) {
        if (std::find(right_children_before.begin(),
                      right_children_before.end(), child_page_id) !=
            right_children_before.end()) {
            auto reparented = reparent_child(
                child_page_id,
                static_cast<std::uint16_t>(left.level - 1),
                right.page_id, left.page_id);
            if (!reparented) {
                return std::unexpected(reparented.error());
            }
        }
    }
    for (const auto child_page_id : internal_children(right)) {
        if (std::find(left_children_before.begin(),
                      left_children_before.end(), child_page_id) !=
            left_children_before.end()) {
            auto reparented = reparent_child(
                child_page_id,
                static_cast<std::uint16_t>(right.level - 1),
                left.page_id, right.page_id);
            if (!reparented) {
                return std::unexpected(reparented.error());
            }
        }
    }

    std::vector<PageUpdate> updates;
    if (auto added = add_page_update(updates, left); !added) {
        return std::unexpected(added.error());
    }
    if (auto added = add_page_update(updates, right); !added) {
        return std::unexpected(added.error());
    }
    if (auto added = add_page_update(updates, parent); !added) {
        return std::unexpected(added.error());
    }
    return write_page_updates(buffer_pool_, std::move(updates));
}

std::expected<void, BTreeError> BTree::reparent_child(
    common::PageId child_page_id, std::uint16_t child_level,
    common::PageId old_parent_page_id,
    std::optional<common::PageId> new_parent_page_id) {
    std::vector<PageUpdate> updates;
    auto page = buffer_pool_.fetch_index_page(child_page_id);
    if (!page) {
        return std::unexpected(BTreeError{page.error()});
    }
    if (child_level == 0) {
        auto child = decode_leaf_page(**page);
        if (!child) {
            return std::unexpected(BTreeError{child.error()});
        }
        if (child->parent_page_id != old_parent_page_id) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        child->parent_page_id = new_parent_page_id;
        if (auto added = add_page_update(updates, *child); !added) {
            return std::unexpected(added.error());
        }
    } else {
        auto child = decode_internal_page(**page);
        if (!child) {
            return std::unexpected(BTreeError{child.error()});
        }
        if (child->level != child_level ||
            child->parent_page_id != old_parent_page_id) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        child->parent_page_id = new_parent_page_id;
        if (auto added = add_page_update(updates, *child); !added) {
            return std::unexpected(added.error());
        }
    }
    return write_page_updates(buffer_pool_, std::move(updates));
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
        return split_leaf_recursively(std::move(leaf),
                                      location->internal_path);
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

std::expected<void, BTreeError> BTree::split_leaf_recursively(
    LeafPage leaf, std::span<const common::PageId> internal_path) {
    if (leaf.page_id == root_page_id_ || internal_path.empty() ||
        internal_path.front() != root_page_id_ ||
        leaf.parent_page_id != internal_path.back()) {
        return std::unexpected(
            BTreeError{BTreeErrorCode::invalid_tree_structure});
    }
    const auto leaf_split = choose_leaf_split(leaf.entries);
    if (!leaf_split) {
        return std::unexpected(BTreeError{IndexPageError::page_full});
    }

    common::PageId right_leaf_page_id;
    {
        auto page = buffer_pool_.allocate_index_leaf_page();
        if (!page) {
            return std::unexpected(BTreeError{page.error()});
        }
        right_leaf_page_id = page->page_id;
    }

    std::vector<LeafEntry> right_entries(
        std::make_move_iterator(
            leaf.entries.begin() +
            static_cast<std::ptrdiff_t>(*leaf_split)),
        std::make_move_iterator(leaf.entries.end()));
    leaf.entries.erase(
        leaf.entries.begin() +
            static_cast<std::ptrdiff_t>(*leaf_split),
        leaf.entries.end());
    const EncodedKey leaf_separator = right_entries.front().key;
    const auto old_next_page_id = leaf.next_page_id;
    leaf.next_page_id = right_leaf_page_id;
    LeafPage right_leaf{
        right_leaf_page_id,
        leaf.parent_page_id,
        old_next_page_id,
        std::move(right_entries),
    };

    std::vector<LeafPage> leaf_pages;
    leaf_pages.push_back(std::move(leaf));
    leaf_pages.push_back(std::move(right_leaf));
    std::vector<InternalPage> internal_pages;

    const auto store_internal = [&internal_pages](InternalPage page) {
        const auto existing = std::find_if(
            internal_pages.begin(), internal_pages.end(),
            [&page](const InternalPage& candidate) {
                return candidate.page_id == page.page_id;
            });
        if (existing == internal_pages.end()) {
            internal_pages.push_back(std::move(page));
        } else {
            *existing = std::move(page);
        }
    };

    const auto reparent_child =
        [this, &leaf_pages, &internal_pages](
            common::PageId child_page_id,
            common::PageId old_parent_page_id,
            common::PageId new_parent_page_id,
            std::uint16_t child_level)
        -> std::expected<void, BTreeError> {
        const auto leaf_page = std::find_if(
            leaf_pages.begin(), leaf_pages.end(),
            [child_page_id](const LeafPage& candidate) {
                return candidate.page_id == child_page_id;
            });
        if (leaf_page != leaf_pages.end()) {
            if (child_level != 0 ||
                leaf_page->parent_page_id != old_parent_page_id) {
                return std::unexpected(
                    BTreeError{BTreeErrorCode::invalid_tree_structure});
            }
            leaf_page->parent_page_id = new_parent_page_id;
            return {};
        }

        const auto internal_page = std::find_if(
            internal_pages.begin(), internal_pages.end(),
            [child_page_id](const InternalPage& candidate) {
                return candidate.page_id == child_page_id;
            });
        if (internal_page != internal_pages.end()) {
            if (child_level == 0 || internal_page->level != child_level ||
                internal_page->parent_page_id != old_parent_page_id) {
                return std::unexpected(
                    BTreeError{BTreeErrorCode::invalid_tree_structure});
            }
            internal_page->parent_page_id = new_parent_page_id;
            return {};
        }

        auto page = buffer_pool_.fetch_index_page(child_page_id);
        if (!page) {
            return std::unexpected(BTreeError{page.error()});
        }
        if (child_level == 0) {
            auto decoded = decode_leaf_page(**page);
            if (!decoded) {
                return std::unexpected(BTreeError{decoded.error()});
            }
            if (decoded->parent_page_id != old_parent_page_id) {
                return std::unexpected(
                    BTreeError{BTreeErrorCode::invalid_tree_structure});
            }
            decoded->parent_page_id = new_parent_page_id;
            leaf_pages.push_back(std::move(*decoded));
            return {};
        }

        auto decoded = decode_internal_page(**page);
        if (!decoded) {
            return std::unexpected(BTreeError{decoded.error()});
        }
        if (decoded->level != child_level ||
            decoded->parent_page_id != old_parent_page_id) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::invalid_tree_structure});
        }
        decoded->parent_page_id = new_parent_page_id;
        internal_pages.push_back(std::move(*decoded));
        return {};
    };

    EncodedKey separator = leaf_separator;
    common::PageId left_child_page_id = leaf_pages.front().page_id;
    common::PageId right_child_page_id = leaf_pages.back().page_id;
    std::optional<common::PageId> new_root_page_id;
    bool propagation_complete = false;

    for (std::size_t path_index = internal_path.size();
         path_index-- > 0;) {
        const common::PageId parent_page_id = internal_path[path_index];
        const std::optional<common::PageId> expected_parent_page_id =
            path_index == 0
                ? std::nullopt
                : std::optional{internal_path[path_index - 1]};
        const std::size_t expected_level_value =
            internal_path.size() - path_index;
        if (expected_level_value >
            std::numeric_limits<std::uint16_t>::max()) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::unsupported_tree_height});
        }

        InternalPage parent;
        {
            auto page = buffer_pool_.fetch_index_page(parent_page_id);
            if (!page) {
                return std::unexpected(BTreeError{page.error()});
            }
            auto decoded = decode_internal_page(**page);
            if (!decoded) {
                return std::unexpected(BTreeError{decoded.error()});
            }
            if (decoded->parent_page_id != expected_parent_page_id ||
                decoded->level != expected_level_value) {
                return std::unexpected(
                    BTreeError{BTreeErrorCode::invalid_tree_structure});
            }
            parent = std::move(*decoded);
        }

        std::size_t insertion_index = 0;
        if (parent.leftmost_child != left_child_page_id) {
            const auto child_entry = std::find_if(
                parent.entries.begin(), parent.entries.end(),
                [left_child_page_id](const InternalEntry& entry) {
                    return entry.right_child == left_child_page_id;
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
        parent.entries.insert(
            parent.entries.begin() +
                static_cast<std::ptrdiff_t>(insertion_index),
            InternalEntry{separator, right_child_page_id});

        const auto parent_size = internal_entries_size(parent.entries);
        if (parent_size && *parent_size <= storage::page_size) {
            store_internal(std::move(parent));
            propagation_complete = true;
            break;
        }

        const auto internal_split = choose_internal_split(parent.entries);
        if (!internal_split) {
            return std::unexpected(BTreeError{IndexPageError::page_full});
        }
        const InternalEntry promoted = parent.entries[*internal_split];

        common::PageId right_internal_page_id;
        {
            auto page = buffer_pool_.allocate_index_internal_page(
                parent.level, promoted.right_child);
            if (!page) {
                return std::unexpected(BTreeError{page.error()});
            }
            right_internal_page_id = page->page_id;
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
            parent.parent_page_id,
            parent.level,
            promoted.right_child,
            std::move(right_internal_entries),
        };

        auto reparented = reparent_child(
            right_internal.leftmost_child, parent.page_id,
            right_internal_page_id,
            static_cast<std::uint16_t>(parent.level - 1));
        if (!reparented) {
            return std::unexpected(reparented.error());
        }
        for (const auto& entry : right_internal.entries) {
            reparented = reparent_child(
                entry.right_child, parent.page_id,
                right_internal_page_id,
                static_cast<std::uint16_t>(parent.level - 1));
            if (!reparented) {
                return std::unexpected(reparented.error());
            }
        }

        if (path_index == 0) {
            if (parent.level ==
                std::numeric_limits<std::uint16_t>::max()) {
                return std::unexpected(
                    BTreeError{BTreeErrorCode::unsupported_tree_height});
            }
            auto new_root = buffer_pool_.allocate_index_internal_page(
                static_cast<std::uint16_t>(parent.level + 1),
                parent.page_id);
            if (!new_root) {
                return std::unexpected(BTreeError{new_root.error()});
            }
            new_root_page_id = new_root->page_id;
            parent.parent_page_id = *new_root_page_id;
            right_internal.parent_page_id = *new_root_page_id;
            InternalPage root{
                *new_root_page_id,
                std::nullopt,
                static_cast<std::uint16_t>(parent.level + 1),
                parent.page_id,
                {{promoted.key, right_internal_page_id}},
            };
            store_internal(std::move(parent));
            store_internal(std::move(right_internal));
            store_internal(std::move(root));
            propagation_complete = true;
            break;
        }

        separator = promoted.key;
        left_child_page_id = parent.page_id;
        right_child_page_id = right_internal_page_id;
        store_internal(std::move(parent));
        store_internal(std::move(right_internal));
    }

    if (!propagation_complete) {
        return std::unexpected(
            BTreeError{BTreeErrorCode::invalid_tree_structure});
    }

    std::vector<PageUpdate> updates;
    for (const auto& page : leaf_pages) {
        if (auto added = add_page_update(updates, page); !added) {
            return std::unexpected(added.error());
        }
    }
    for (const auto& page : internal_pages) {
        if (auto added = add_page_update(updates, page); !added) {
            return std::unexpected(added.error());
        }
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

}  // namespace minidb::index
