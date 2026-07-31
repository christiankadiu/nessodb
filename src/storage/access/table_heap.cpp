#include "storage/access/table_heap.hpp"

#include <utility>

namespace minidb::storage {

std::expected<TableHeap, TableHeapError> TableHeap::create(
    BufferPool& buffer_pool, catalog::TableSchema schema) {
    auto allocated = buffer_pool.allocate_heap_page();
    if (!allocated) {
        return std::unexpected(TableHeapError{allocated.error()});
    }
    return TableHeap{buffer_pool, std::move(schema), allocated->page_id};
}

common::PageId TableHeap::first_page_id() const noexcept {
    return first_page_id_;
}

std::expected<RecordId, TableHeapError> TableHeap::insert(const Row& row) {
    auto record = encode_record(row, schema_);
    if (!record) {
        return std::unexpected(TableHeapError{record.error()});
    }

    auto page = buffer_pool_.fetch_heap_page_for_write(first_page_id_);
    if (!page) {
        return std::unexpected(TableHeapError{page.error()});
    }
    auto slotted_page = SlottedPage::open(**page);
    if (!slotted_page) {
        return std::unexpected(TableHeapError{slotted_page.error()});
    }
    auto slot_id = slotted_page->insert(*record);
    if (!slot_id) {
        return std::unexpected(TableHeapError{slot_id.error()});
    }
    return RecordId{first_page_id_, *slot_id};
}

TableHeap::TableHeap(BufferPool& buffer_pool, catalog::TableSchema schema,
                     common::PageId first_page_id)
    : buffer_pool_(buffer_pool), schema_(std::move(schema)), first_page_id_(first_page_id) {}

}  // namespace minidb::storage
