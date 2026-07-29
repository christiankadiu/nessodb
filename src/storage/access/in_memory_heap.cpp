#include "storage/access/in_memory_heap.hpp"

#include <utility>

namespace minidb::storage {

std::expected<void, HeapError> InMemoryHeap::create_table(common::TableId table_id) {
    if (!table_id.is_valid()) {
        return std::unexpected(HeapError::invalid_table_id);
    }
    if (find_table(table_id) != nullptr) {
        return std::unexpected(HeapError::table_already_exists);
    }

    tables_.push_back(TableData{table_id, {}});
    return {};
}

std::expected<void, HeapError> InMemoryHeap::insert(common::TableId table_id, Row row) {
    TableData* table = find_table(table_id);
    if (table == nullptr) {
        return std::unexpected(HeapError::table_not_found);
    }

    table->rows.push_back(std::move(row));
    return {};
}

std::expected<std::span<const Row>, HeapError> InMemoryHeap::scan(
    common::TableId table_id) const noexcept {
    const TableData* table = find_table(table_id);
    if (table == nullptr) {
        return std::unexpected(HeapError::table_not_found);
    }
    return std::span<const Row>{table->rows};
}

InMemoryHeap::TableData* InMemoryHeap::find_table(common::TableId table_id) noexcept {
    for (auto& table : tables_) {
        if (table.id == table_id) {
            return &table;
        }
    }
    return nullptr;
}

const InMemoryHeap::TableData* InMemoryHeap::find_table(
    common::TableId table_id) const noexcept {
    for (const auto& table : tables_) {
        if (table.id == table_id) {
            return &table;
        }
    }
    return nullptr;
}

}  // namespace minidb::storage
