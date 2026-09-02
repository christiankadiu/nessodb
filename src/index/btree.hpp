#pragma once

#include "common/page_id.hpp"
#include "index/btree_page.hpp"
#include "storage/access/record_id.hpp"
#include "storage/buffer/buffer_pool.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace minidb::index {

enum class BTreeErrorCode {
    invalid_root_page_id,
    invalid_record_id,
    duplicate_entry,
    unsupported_tree_height,
    invalid_tree_structure,
    leaf_chain_cycle,
};

using BTreeError = std::variant<storage::BufferPoolError, IndexPageError,
                                BTreeErrorCode>;

class BTree {
public:
    [[nodiscard]] static std::expected<BTree, BTreeError> create(
        storage::BufferPool& buffer_pool);
    [[nodiscard]] static std::expected<BTree, BTreeError> open(
        storage::BufferPool& buffer_pool, common::PageId root_page_id);

    [[nodiscard]] common::PageId root_page_id() const noexcept;
    [[nodiscard]] std::expected<std::vector<storage::RecordId>, BTreeError>
    find(std::span<const std::byte> key) const;
    [[nodiscard]] std::expected<void, BTreeError> insert(
        std::span<const std::byte> key, storage::RecordId record_id);

private:
    struct LeafLocation {
        common::PageId page_id;
        std::optional<common::PageId> parent_page_id;
    };

    BTree(storage::BufferPool& buffer_pool,
          common::PageId root_page_id) noexcept;
    [[nodiscard]] std::expected<LeafLocation, BTreeError> locate_leaf(
        std::span<const std::byte> key, bool right_bias) const;
    [[nodiscard]] std::expected<void, BTreeError> split_root_leaf(
        LeafPage leaf);

    storage::BufferPool& buffer_pool_;
    common::PageId root_page_id_;
};

}  // namespace minidb::index
