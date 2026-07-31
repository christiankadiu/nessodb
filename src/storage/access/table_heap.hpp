#pragma once

#include "catalog/schema.hpp"
#include "common/page_id.hpp"
#include "storage/access/record_id.hpp"
#include "storage/access/row.hpp"
#include "storage/buffer/buffer_pool.hpp"
#include "storage/page/slotted_page.hpp"
#include "storage/record/record_codec.hpp"

#include <expected>
#include <variant>

namespace minidb::storage {

enum class TableHeapErrorCode {
    record_too_large,
    tail_page_already_linked,
    invalid_first_page_id,
    page_chain_cycle,
};

using TableHeapError =
    std::variant<BufferPoolError, RecordEncodeError, SlottedPageError, TableHeapErrorCode>;

class TableHeap {
public:
    [[nodiscard]] static std::expected<TableHeap, TableHeapError> create(
        BufferPool& buffer_pool, catalog::TableSchema schema);
    [[nodiscard]] static std::expected<TableHeap, TableHeapError> open(
        BufferPool& buffer_pool, catalog::TableSchema schema,
        common::PageId first_page_id);

    [[nodiscard]] common::PageId first_page_id() const noexcept;
    [[nodiscard]] std::expected<RecordId, TableHeapError> insert(const Row& row);

private:
    TableHeap(BufferPool& buffer_pool, catalog::TableSchema schema,
              common::PageId first_page_id, common::PageId last_page_id);

    BufferPool& buffer_pool_;
    catalog::TableSchema schema_;
    common::PageId first_page_id_;
    common::PageId last_page_id_;
};

}  // namespace minidb::storage
