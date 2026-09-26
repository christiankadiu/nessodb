#include "storage/access/in_memory_heap.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>

namespace {

using nessodb::common::TableId;
using nessodb::storage::HeapError;
using nessodb::storage::InMemoryHeap;
using nessodb::storage::InMemoryRowId;
using nessodb::storage::Row;

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

void test_create_table() {
    InMemoryHeap heap;

    const auto invalid = heap.create_table(TableId{});
    expect(!invalid && invalid.error() == HeapError::invalid_table_id,
           "invalid table identifier is rejected");

    const auto created = heap.create_table(TableId{1});
    expect(created.has_value(), "table heap is created");

    const auto duplicate = heap.create_table(TableId{1});
    expect(!duplicate && duplicate.error() == HeapError::table_already_exists,
           "duplicate table heap is rejected");
}

void test_insert_and_scan() {
    InMemoryHeap heap;
    const TableId table_id{1};
    const auto created = heap.create_table(table_id);
    expect(created.has_value(), "table heap is created before insertion");

    const auto first = heap.insert(table_id, Row{{std::int64_t{1}, std::string{"Alice"}}});
    const auto second = heap.insert(table_id, Row{{std::int64_t{2}, std::string{"Bob"}}});
    expect(first == InMemoryRowId{0} && second == InMemoryRowId{1},
           "rows receive stable identifiers");
    if (!first || !second) {
        return;
    }

    const auto rows = heap.scan(table_id);
    expect(rows.has_value(), "table heap is scanned");
    if (rows) {
        expect(rows->size() == 2, "scan returns every row");
        if (rows->size() == 2) {
            expect(std::get<std::int64_t>((*rows)[0].values[0]) == 1,
                   "first row preserves its integer value");
            expect(std::get<std::string>((*rows)[1].values[1]) == "Bob",
                   "second row preserves its text value");
        }
    }

    const auto records = heap.scan_records(table_id);
    expect(records && records->size() == 2 &&
               (*records)[0].row_id == *first &&
               (*records)[1].row_id == *second,
           "record scan returns row identifiers");

    const Row updated{{std::int64_t{10}, std::string{"Updated"}}};
    expect(heap.update(table_id, *first, updated).has_value(),
           "row is updated by identifier");
    const auto updated_records = heap.scan_records(table_id);
    expect(updated_records && updated_records->size() == 2 &&
               updated_records->front().row_id == *first &&
               updated_records->front().row.values == updated.values &&
               (*updated_records)[1].row_id == *second,
           "update preserves row identifiers and surrounding rows");

    expect(heap.erase(table_id, *first).has_value(),
           "row is deleted by identifier");
    const auto remaining = heap.scan_records(table_id);
    expect(remaining && remaining->size() == 1 &&
               remaining->front().row_id == *second &&
               std::get<std::string>(
                   remaining->front().row.values[1]) == "Bob",
           "scans omit deleted rows without changing other identifiers");

    expect(heap.restore(table_id, *first, updated).has_value(),
           "a deleted row is restored by identifier");
    const auto restored = heap.scan_records(table_id);
    expect(restored && restored->size() == 2 &&
               restored->front().row_id == *first &&
               restored->front().row.values == updated.values,
           "restoration preserves the original row identifier and value");
    const auto occupied = heap.restore(table_id, *first, updated);
    expect(!occupied && occupied.error() == HeapError::row_already_exists,
           "restoring an occupied row identifier is rejected");
    expect(heap.erase(table_id, *first).has_value(),
           "restored row can be deleted again");

    const auto replacement = heap.insert(
        table_id, Row{{std::int64_t{3}, std::string{"Carol"}}});
    expect(replacement == first, "insertion reuses a deleted row slot");
    const auto repeated = heap.erase(table_id, *second);
    expect(repeated.has_value(), "remaining row can be deleted");
    const auto missing_row = heap.erase(table_id, *second);
    expect(!missing_row && missing_row.error() == HeapError::row_not_found,
           "deleting the same row twice is rejected");
    const auto invalid_row = heap.erase(table_id, InMemoryRowId{});
    expect(!invalid_row && invalid_row.error() == HeapError::invalid_row_id,
           "invalid row identifier is rejected");
    const auto update_missing = heap.update(table_id, *second, Row{});
    expect(!update_missing && update_missing.error() == HeapError::row_not_found,
           "updating a deleted row is rejected");
    const auto update_invalid = heap.update(table_id, InMemoryRowId{}, Row{});
    expect(!update_invalid && update_invalid.error() == HeapError::invalid_row_id,
           "updating an invalid row identifier is rejected");

    const auto missing = heap.insert(TableId{2}, Row{});
    expect(!missing && missing.error() == HeapError::table_not_found,
           "insertion into an unknown table is rejected");
    const auto update_unknown = heap.update(TableId{2}, *first, Row{});
    expect(!update_unknown && update_unknown.error() == HeapError::table_not_found,
           "update in an unknown table is rejected");
}

}  // namespace

int main() {
    test_create_table();
    test_insert_and_scan();

    if (failures != 0) {
        std::cerr << failures << " heap assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
