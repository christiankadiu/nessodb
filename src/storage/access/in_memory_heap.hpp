#pragma once

#include "common/table_id.hpp"
#include "storage/access/row.hpp"

#include <cstddef>
#include <deque>
#include <expected>
#include <limits>
#include <optional>
#include <vector>

namespace minidb::storage {

enum class HeapError {
    invalid_table_id,
    table_already_exists,
    table_not_found,
    invalid_row_id,
    row_not_found,
};

struct InMemoryRowId {
    static constexpr std::size_t invalid_value =
        std::numeric_limits<std::size_t>::max();

    std::size_t value{invalid_value};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return value != invalid_value;
    }

    friend bool operator==(const InMemoryRowId&, const InMemoryRowId&) = default;
};

struct InMemoryStoredRow {
    InMemoryRowId row_id;
    Row row;
};

class InMemoryHeap {
public:
    [[nodiscard]] std::expected<void, HeapError> create_table(common::TableId table_id);
    [[nodiscard]] std::expected<InMemoryRowId, HeapError> insert(
        common::TableId table_id, Row row);
    [[nodiscard]] std::expected<void, HeapError> erase(
        common::TableId table_id, InMemoryRowId row_id);
    [[nodiscard]] std::expected<std::vector<Row>, HeapError> scan(
        common::TableId table_id) const;
    [[nodiscard]] std::expected<std::vector<InMemoryStoredRow>, HeapError>
    scan_records(common::TableId table_id) const;

private:
    struct TableData {
        common::TableId id;
        std::vector<std::optional<Row>> rows;
    };

    [[nodiscard]] TableData* find_table(common::TableId table_id) noexcept;
    [[nodiscard]] const TableData* find_table(common::TableId table_id) const noexcept;

    std::deque<TableData> tables_;
};

}  // namespace minidb::storage
