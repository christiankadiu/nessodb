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

std::expected<InMemoryRowId, HeapError> InMemoryHeap::insert(
    common::TableId table_id, Row row) {
    TableData* table = find_table(table_id);
    if (table == nullptr) {
        return std::unexpected(HeapError::table_not_found);
    }

    for (std::size_t index = 0; index < table->rows.size(); ++index) {
        if (!table->rows[index]) {
            table->rows[index] = std::move(row);
            return InMemoryRowId{index};
        }
    }
    table->rows.push_back(std::move(row));
    return InMemoryRowId{table->rows.size() - 1};
}

std::expected<void, HeapError> InMemoryHeap::erase(
    common::TableId table_id, InMemoryRowId row_id) {
    TableData* table = find_table(table_id);
    if (table == nullptr) {
        return std::unexpected(HeapError::table_not_found);
    }
    if (!row_id.is_valid()) {
        return std::unexpected(HeapError::invalid_row_id);
    }
    if (row_id.value >= table->rows.size() || !table->rows[row_id.value]) {
        return std::unexpected(HeapError::row_not_found);
    }

    table->rows[row_id.value].reset();
    return {};
}

std::expected<std::vector<Row>, HeapError> InMemoryHeap::scan(
    common::TableId table_id) const {
    const TableData* table = find_table(table_id);
    if (table == nullptr) {
        return std::unexpected(HeapError::table_not_found);
    }

    std::vector<Row> rows;
    rows.reserve(table->rows.size());
    for (const auto& row : table->rows) {
        if (row) {
            rows.push_back(*row);
        }
    }
    return rows;
}

std::expected<std::vector<InMemoryStoredRow>, HeapError>
InMemoryHeap::scan_records(common::TableId table_id) const {
    const TableData* table = find_table(table_id);
    if (table == nullptr) {
        return std::unexpected(HeapError::table_not_found);
    }

    std::vector<InMemoryStoredRow> rows;
    rows.reserve(table->rows.size());
    for (std::size_t index = 0; index < table->rows.size(); ++index) {
        if (table->rows[index]) {
            rows.push_back(
                InMemoryStoredRow{InMemoryRowId{index}, *table->rows[index]});
        }
    }
    return rows;
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
