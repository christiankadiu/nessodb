#pragma once

#include "common/page_id.hpp"
#include "index/btree_page.hpp"
#include "storage/access/record_id.hpp"
#include "storage/buffer/buffer_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <unordered_set>
#include <variant>
#include <vector>

namespace nessodb::index {

enum class BTreeErrorCode {
    invalid_root_page_id,
    invalid_record_id,
    duplicate_entry,
    entry_not_found,
    unsupported_tree_height,
    invalid_tree_structure,
    leaf_chain_cycle,
    invalid_range,
};

using BTreeError = std::variant<storage::BufferPoolError, IndexPageError,
                                BTreeErrorCode>;

struct BTreeKeyBound {
    EncodedKey key;
    bool inclusive{true};
};

struct BTreeRange {
    std::optional<BTreeKeyBound> lower;
    std::optional<BTreeKeyBound> upper;
};

class BTreeCursor {
public:
    [[nodiscard]] std::expected<std::optional<LeafEntry>, BTreeError> next();

private:
    friend class BTree;

    BTreeCursor(storage::BufferPool& buffer_pool, LeafPage leaf,
                std::size_t entry_index,
                std::optional<BTreeKeyBound> lower_bound,
                std::optional<BTreeKeyBound> upper_bound);

    storage::BufferPool* buffer_pool_;
    std::optional<LeafPage> leaf_;
    std::size_t entry_index_;
    std::optional<BTreeKeyBound> lower_bound_;
    std::optional<BTreeKeyBound> upper_bound_;
    std::optional<EncodedKey> last_key_;
    std::unordered_set<std::uint64_t> visited_pages_;
    bool requires_parent_;
};

class BTree {
public:
    [[nodiscard]] static std::expected<BTree, BTreeError> create(
        storage::BufferPool& buffer_pool);
    [[nodiscard]] static std::expected<BTree, BTreeError> open(
        storage::BufferPool& buffer_pool, common::PageId root_page_id);

    [[nodiscard]] common::PageId root_page_id() const noexcept;
    [[nodiscard]] std::expected<std::vector<storage::RecordId>, BTreeError>
    find(std::span<const std::byte> key) const;
    [[nodiscard]] std::expected<BTreeCursor, BTreeError> scan(
        BTreeRange range = {}) const;
    [[nodiscard]] std::expected<void, BTreeError> erase(
        std::span<const std::byte> key, storage::RecordId record_id);
    [[nodiscard]] std::expected<void, BTreeError> insert(
        std::span<const std::byte> key, storage::RecordId record_id);

private:
    struct LeafLocation {
        common::PageId page_id;
        std::vector<common::PageId> internal_path;
    };

    BTree(storage::BufferPool& buffer_pool,
          common::PageId root_page_id) noexcept;
    [[nodiscard]] std::expected<LeafLocation, BTreeError> locate_leaf(
        std::span<const std::byte> key, bool right_bias) const;
    [[nodiscard]] std::expected<LeafLocation, BTreeError>
    locate_leftmost_leaf() const;
    [[nodiscard]] std::expected<void, BTreeError> rebalance_leaf_after_erase(
        LeafPage leaf);
    [[nodiscard]] std::expected<void, BTreeError>
    rebalance_internal_after_erase(InternalPage page);
    [[nodiscard]] std::expected<void, BTreeError> reparent_child(
        common::PageId child_page_id, std::uint16_t child_level,
        common::PageId old_parent_page_id,
        std::optional<common::PageId> new_parent_page_id);
    [[nodiscard]] std::expected<void, BTreeError> split_root_leaf(
        LeafPage leaf);
    [[nodiscard]] std::expected<void, BTreeError> split_leaf_recursively(
        LeafPage leaf, std::span<const common::PageId> internal_path);

    storage::BufferPool& buffer_pool_;
    common::PageId root_page_id_;
};

}  // namespace nessodb::index
