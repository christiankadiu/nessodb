#pragma once

#include "common/table_id.hpp"
#include "storage/access/row.hpp"

#include <deque>
#include <expected>
#include <span>
#include <vector>

namespace minidb::storage {

enum class HeapError {
    invalid_table_id,
    table_already_exists,
    table_not_found,
};

class InMemoryHeap {
public:
    [[nodiscard]] std::expected<void, HeapError> create_table(common::TableId table_id);
    [[nodiscard]] std::expected<void, HeapError> insert(common::TableId table_id, Row row);
    [[nodiscard]] std::expected<std::span<const Row>, HeapError> scan(
        common::TableId table_id) const noexcept;

private:
    struct TableData {
        common::TableId id;
        std::vector<Row> rows;
    };

    [[nodiscard]] TableData* find_table(common::TableId table_id) noexcept;
    [[nodiscard]] const TableData* find_table(common::TableId table_id) const noexcept;

    std::deque<TableData> tables_;
};

}  // namespace minidb::storage
