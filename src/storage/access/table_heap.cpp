#include "storage/access/table_heap.hpp"

#include "storage/page/page.hpp"

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
    constexpr std::size_t maximum_record_size =
        page_size - heap_page_header_size - slot_entry_size;
    if (record->size() > maximum_record_size) {
        return std::unexpected(TableHeapError{TableHeapErrorCode::record_too_large});
    }

    {
        auto page = buffer_pool_.fetch_heap_page_for_write(last_page_id_);
        if (!page) {
            return std::unexpected(TableHeapError{page.error()});
        }
        auto slotted_page = SlottedPage::open(**page);
        if (!slotted_page) {
            return std::unexpected(TableHeapError{slotted_page.error()});
        }
        auto slot_id = slotted_page->insert(*record);
        if (slot_id) {
            return RecordId{last_page_id_, *slot_id};
        }
        if (slot_id.error() != SlottedPageError::page_full) {
            return std::unexpected(TableHeapError{slot_id.error()});
        }
        if (slotted_page->next_page_id()) {
            return std::unexpected(
                TableHeapError{TableHeapErrorCode::tail_page_already_linked});
        }
    }

    common::PageId new_page_id;
    {
        auto allocated = buffer_pool_.allocate_heap_page();
        if (!allocated) {
            return std::unexpected(TableHeapError{allocated.error()});
        }
        new_page_id = allocated->page_id;
    }

    {
        auto old_tail = buffer_pool_.fetch_heap_page_for_write(last_page_id_);
        if (!old_tail) {
            return std::unexpected(TableHeapError{old_tail.error()});
        }
        auto slotted_page = SlottedPage::open(**old_tail);
        if (!slotted_page) {
            return std::unexpected(TableHeapError{slotted_page.error()});
        }
        auto linked = slotted_page->set_next_page_id(new_page_id);
        if (!linked) {
            return std::unexpected(TableHeapError{linked.error()});
        }
    }
    last_page_id_ = new_page_id;

    auto new_tail = buffer_pool_.fetch_heap_page_for_write(last_page_id_);
    if (!new_tail) {
        return std::unexpected(TableHeapError{new_tail.error()});
    }
    auto slotted_page = SlottedPage::open(**new_tail);
    if (!slotted_page) {
        return std::unexpected(TableHeapError{slotted_page.error()});
    }
    auto slot_id = slotted_page->insert(*record);
    if (!slot_id) {
        return std::unexpected(TableHeapError{slot_id.error()});
    }
    return RecordId{last_page_id_, *slot_id};
}

TableHeap::TableHeap(BufferPool& buffer_pool, catalog::TableSchema schema,
                     common::PageId first_page_id)
    : buffer_pool_(buffer_pool),
      schema_(std::move(schema)),
      first_page_id_(first_page_id),
      last_page_id_(first_page_id) {}

}  // namespace minidb::storage
