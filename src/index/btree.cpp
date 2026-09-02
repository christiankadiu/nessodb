#include "index/btree.hpp"

#include "index/key_codec.hpp"
#include "storage/page/page.hpp"

#include <algorithm>
#include <compare>
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
    const auto leaf = decode_leaf_page(**root);
    if (!leaf) {
        if (leaf.error() == IndexPageError::wrong_page_type) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::unsupported_tree_height});
        }
        return std::unexpected(BTreeError{leaf.error()});
    }
    if (leaf->parent_page_id) {
        return std::unexpected(
            BTreeError{BTreeErrorCode::invalid_root_page_id});
    }
    return BTree{buffer_pool, root_page_id};
}

BTree::BTree(storage::BufferPool& buffer_pool,
             common::PageId root_page_id) noexcept
    : buffer_pool_(buffer_pool), root_page_id_(root_page_id) {}

common::PageId BTree::root_page_id() const noexcept {
    return root_page_id_;
}

std::expected<std::vector<storage::RecordId>, BTreeError> BTree::find(
    std::span<const std::byte> key) const {
    auto valid_key = validate_key(key);
    if (!valid_key) {
        return std::unexpected(BTreeError{valid_key.error()});
    }

    auto root = buffer_pool_.fetch_index_page(root_page_id_);
    if (!root) {
        return std::unexpected(BTreeError{root.error()});
    }
    auto leaf = decode_leaf_page(**root);
    if (!leaf) {
        if (leaf.error() == IndexPageError::wrong_page_type) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::unsupported_tree_height});
        }
        return std::unexpected(BTreeError{leaf.error()});
    }
    if (leaf->parent_page_id) {
        return std::unexpected(
            BTreeError{BTreeErrorCode::invalid_root_page_id});
    }

    const auto first = std::lower_bound(
        leaf->entries.begin(), leaf->entries.end(), key,
        [](const LeafEntry& entry, std::span<const std::byte> candidate) {
            return compare_encoded_keys(entry.key, candidate) ==
                   std::strong_ordering::less;
        });
    std::vector<storage::RecordId> matches;
    for (auto entry = first;
         entry != leaf->entries.end() &&
         compare_encoded_keys(entry->key, key) ==
             std::strong_ordering::equal;
         ++entry) {
        matches.push_back(entry->record_id);
    }
    return matches;
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

    auto root = buffer_pool_.fetch_index_page(root_page_id_);
    if (!root) {
        return std::unexpected(BTreeError{root.error()});
    }
    auto leaf = decode_leaf_page(**root);
    if (!leaf) {
        if (leaf.error() == IndexPageError::wrong_page_type) {
            return std::unexpected(
                BTreeError{BTreeErrorCode::unsupported_tree_height});
        }
        return std::unexpected(BTreeError{leaf.error()});
    }
    if (leaf->parent_page_id) {
        return std::unexpected(
            BTreeError{BTreeErrorCode::invalid_root_page_id});
    }

    LeafEntry new_entry{EncodedKey{key.begin(), key.end()}, record_id};
    const auto position = std::lower_bound(
        leaf->entries.begin(), leaf->entries.end(), new_entry, entry_less);
    if (position != leaf->entries.end() && *position == new_entry) {
        return std::unexpected(BTreeError{BTreeErrorCode::duplicate_entry});
    }
    leaf->entries.insert(position, std::move(new_entry));

    storage::PageBuffer encoded{};
    auto encoded_leaf = encode_leaf_page(encoded, *leaf);
    if (!encoded_leaf) {
        return std::unexpected(BTreeError{encoded_leaf.error()});
    }

    auto writable = buffer_pool_.fetch_index_page_for_write(root_page_id_);
    if (!writable) {
        return std::unexpected(BTreeError{writable.error()});
    }
    **writable = std::move(encoded);
    return {};
}

}  // namespace minidb::index
