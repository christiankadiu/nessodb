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

using TableHeapError =
    std::variant<BufferPoolError, RecordEncodeError, SlottedPageError>;

class TableHeap {
public:
    [[nodiscard]] static std::expected<TableHeap, TableHeapError> create(
        BufferPool& buffer_pool, catalog::TableSchema schema);

    [[nodiscard]] common::PageId first_page_id() const noexcept;
    [[nodiscard]] std::expected<RecordId, TableHeapError> insert(const Row& row);

private:
    TableHeap(BufferPool& buffer_pool, catalog::TableSchema schema,
              common::PageId first_page_id);

    BufferPool& buffer_pool_;
    catalog::TableSchema schema_;
    common::PageId first_page_id_;
};

}  // namespace minidb::storage
