#include "storage/access/table_heap.hpp"

#include "catalog/schema.hpp"
#include "storage/access/row.hpp"
#include "storage/buffer/buffer_pool.hpp"
#include "storage/io/database_file.hpp"
#include "storage/page/slotted_page.hpp"
#include "storage/record/record_codec.hpp"
#include "types/logical_type.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>

namespace {

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("nessodb-table-heap-test-" + std::to_string(suffix));
        std::filesystem::create_directory(path_);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void test_insert_rows() {
    TemporaryDirectory directory;
    auto database = nessodb::storage::DatabaseFile::create(
        directory.path() / "database.mdb");
    expect(database.has_value(), "table heap database is created");
    if (!database) {
        return;
    }
    nessodb::storage::BufferPool buffer_pool(*database, 2);
    const nessodb::catalog::TableSchema schema{
        "items",
        {
            {"id", nessodb::types::LogicalType::integer},
            {"name", nessodb::types::LogicalType::text},
        },
    };

    auto table_heap = nessodb::storage::TableHeap::create(buffer_pool, schema);
    expect(table_heap.has_value(), "table heap is created");
    if (!table_heap) {
        return;
    }
    expect(table_heap->first_page_id() == nessodb::common::PageId{1},
           "table heap exposes its root page");

    const nessodb::storage::Row first{{std::int64_t{1}, std::string{"Alice"}}};
    const nessodb::storage::Row second{{std::int64_t{2}, std::string{"Bob"}}};
    const auto first_id = table_heap->insert(first);
    const auto second_id = table_heap->insert(second);
    expect(first_id == nessodb::storage::RecordId{nessodb::common::PageId{1},
                                                 nessodb::storage::SlotId{0}},
           "first row receives root page and slot zero");
    expect(second_id == nessodb::storage::RecordId{nessodb::common::PageId{1},
                                                  nessodb::storage::SlotId{1}},
           "second row receives the next slot");
    expect(buffer_pool.flush().has_value(), "table heap rows are flushed");

    auto page = database->read_heap_page(table_heap->first_page_id());
    expect(page.has_value(), "table heap root page is persisted");
    if (!page || !first_id) {
        return;
    }
    auto slotted_page = nessodb::storage::SlottedPage::open(*page);
    expect(slotted_page.has_value(), "persisted table heap page is valid");
    if (!slotted_page) {
        return;
    }
    auto record = slotted_page->read(first_id->slot_id);
    expect(record.has_value(), "inserted record is readable from its slot");
    if (!record) {
        return;
    }
    const auto decoded = nessodb::storage::decode_record(*record, schema);
    expect(decoded && decoded->values == first.values,
           "inserted row round-trips through table heap storage");
}

void test_grow_table_heap() {
    TemporaryDirectory directory;
    auto database = nessodb::storage::DatabaseFile::create(
        directory.path() / "growth.mdb");
    expect(database.has_value(), "growing table heap database is created");
    if (!database) {
        return;
    }
    nessodb::storage::BufferPool buffer_pool(*database, 1);
    const nessodb::catalog::TableSchema schema{
        "documents",
        {{"body", nessodb::types::LogicalType::text}},
    };
    auto table_heap = nessodb::storage::TableHeap::create(buffer_pool, schema);
    if (!table_heap) {
        expect(false, "growing table heap is created");
        return;
    }

    const nessodb::storage::Row row{{std::string(2000, 'x')}};
    const auto first = table_heap->insert(row);
    const auto second = table_heap->insert(row);
    const auto third = table_heap->insert(row);
    expect(first && first->page_id == nessodb::common::PageId{1} &&
               first->slot_id == nessodb::storage::SlotId{0},
           "first large row is stored on root page");
    expect(second && second->page_id == nessodb::common::PageId{1} &&
               second->slot_id == nessodb::storage::SlotId{1},
           "second large row fills root page");
    expect(third && third->page_id == nessodb::common::PageId{2} &&
               third->slot_id == nessodb::storage::SlotId{0},
           "third large row grows table heap onto next page");
    expect(database->header().page_count == 3,
           "table heap growth allocates exactly one additional page");
    expect(buffer_pool.flush().has_value(), "growing table heap is flushed");

    auto root_buffer = database->read_heap_page(nessodb::common::PageId{1});
    expect(root_buffer.has_value(), "table heap root page is read after growth");
    if (root_buffer) {
        auto root = nessodb::storage::SlottedPage::open(*root_buffer);
        expect(root && root->next_page_id() == nessodb::common::PageId{2},
               "root page links to allocated successor");
    }

    const auto pages_before_oversized_row = database->header().page_count;
    const nessodb::storage::Row oversized{{std::string(5000, 'y')}};
    const auto rejected = table_heap->insert(oversized);
    expect(!rejected &&
               std::holds_alternative<nessodb::storage::TableHeapErrorCode>(
                   rejected.error()) &&
               std::get<nessodb::storage::TableHeapErrorCode>(rejected.error()) ==
                   nessodb::storage::TableHeapErrorCode::record_too_large,
           "row larger than an empty heap page is rejected");
    expect(database->header().page_count == pages_before_oversized_row,
           "oversized row does not allocate an orphan page");
}

void test_reopen_table_heap() {
    TemporaryDirectory directory;
    auto database = nessodb::storage::DatabaseFile::create(
        directory.path() / "reopen.mdb");
    if (!database) {
        expect(false, "reopen test database is created");
        return;
    }
    const nessodb::catalog::TableSchema schema{
        "documents",
        {{"body", nessodb::types::LogicalType::text}},
    };
    const nessodb::storage::Row row{{std::string(2000, 'z')}};

    {
        nessodb::storage::BufferPool buffer_pool(*database, 1);
        auto table_heap = nessodb::storage::TableHeap::create(buffer_pool, schema);
        if (!table_heap) {
            expect(false, "reopen test table heap is created");
            return;
        }
        expect(table_heap->insert(row).has_value(), "first reopen row is inserted");
        expect(table_heap->insert(row).has_value(), "second reopen row is inserted");
        expect(table_heap->insert(row).has_value(), "third reopen row grows heap");
        expect(buffer_pool.flush().has_value(), "reopen test table heap is flushed");
    }

    {
        nessodb::storage::BufferPool buffer_pool(*database, 1);
        auto reopened = nessodb::storage::TableHeap::open(
            buffer_pool, schema, nessodb::common::PageId{1});
        expect(reopened.has_value(), "existing multipage table heap is reopened");
        if (!reopened) {
            return;
        }
        const auto inserted = reopened->insert(row);
        expect(inserted && inserted->page_id == nessodb::common::PageId{2} &&
                   inserted->slot_id == nessodb::storage::SlotId{1},
               "reopened table heap inserts into reconstructed tail page");
        expect(buffer_pool.flush().has_value(), "reopened table heap is flushed");
    }

    auto second_page = database->read_heap_page(nessodb::common::PageId{2});
    if (!second_page) {
        expect(false, "second heap page is read for cycle test");
        return;
    }
    auto slotted_page = nessodb::storage::SlottedPage::open(*second_page);
    if (!slotted_page) {
        expect(false, "second heap page is valid for cycle test");
        return;
    }
    expect(slotted_page->set_next_page_id(nessodb::common::PageId{1}).has_value(),
           "cycle is written for validation test");
    expect(database->write_heap_page(nessodb::common::PageId{2}, *second_page).has_value(),
           "cyclic heap page is persisted for validation test");

    nessodb::storage::BufferPool buffer_pool(*database, 1);
    const auto cyclic = nessodb::storage::TableHeap::open(
        buffer_pool, schema, nessodb::common::PageId{1});
    expect(!cyclic &&
               std::holds_alternative<nessodb::storage::TableHeapErrorCode>(
                   cyclic.error()) &&
               std::get<nessodb::storage::TableHeapErrorCode>(cyclic.error()) ==
                   nessodb::storage::TableHeapErrorCode::page_chain_cycle,
           "cyclic table heap chain is rejected");
}

void test_scan_table_heap() {
    TemporaryDirectory directory;
    auto database = nessodb::storage::DatabaseFile::create(
        directory.path() / "scan.mdb");
    if (!database) {
        expect(false, "scan test database is created");
        return;
    }
    nessodb::storage::BufferPool buffer_pool(*database, 1);
    const nessodb::catalog::TableSchema schema{
        "documents",
        {{"body", nessodb::types::LogicalType::text}},
    };
    auto table_heap = nessodb::storage::TableHeap::create(buffer_pool, schema);
    if (!table_heap) {
        expect(false, "scan test table heap is created");
        return;
    }

    const auto empty_scan = table_heap->scan();
    expect(empty_scan && empty_scan->empty(), "empty table heap scan has no rows");

    const nessodb::storage::Row first{{std::string(2000, 'a')}};
    const nessodb::storage::Row second{{std::string(2000, 'b')}};
    const nessodb::storage::Row third{{std::string(2000, 'c')}};
    expect(table_heap->insert(first).has_value(), "first scan row is inserted");
    expect(table_heap->insert(second).has_value(), "second scan row is inserted");
    expect(table_heap->insert(third).has_value(), "third scan row grows heap");

    const auto rows = table_heap->scan();
    expect(rows && rows->size() == 3, "scan visits rows across all heap pages");
    if (rows && rows->size() == 3) {
        expect((*rows)[0].values == first.values &&
                   (*rows)[1].values == second.values &&
                   (*rows)[2].values == third.values,
               "scan preserves heap page and slot order");
    }

    const auto records = table_heap->scan_records();
    expect(records && records->size() == 3 &&
               (*records)[0].record_id == nessodb::storage::RecordId{
                   nessodb::common::PageId{1}, nessodb::storage::SlotId{0}} &&
               (*records)[1].record_id == nessodb::storage::RecordId{
                   nessodb::common::PageId{1}, nessodb::storage::SlotId{1}} &&
               (*records)[2].record_id == nessodb::storage::RecordId{
                   nessodb::common::PageId{2}, nessodb::storage::SlotId{0}},
           "record scan preserves each row identifier");
}

void test_delete_rows() {
    TemporaryDirectory directory;
    auto database = nessodb::storage::DatabaseFile::create(
        directory.path() / "delete.mdb");
    if (!database) {
        expect(false, "delete test database is created");
        return;
    }
    const nessodb::catalog::TableSchema schema{
        "documents",
        {{"body", nessodb::types::LogicalType::text}},
    };
    const nessodb::storage::Row first{{std::string(2000, 'a')}};
    const nessodb::storage::Row second{{std::string(2000, 'b')}};
    const nessodb::storage::Row third{{std::string(2000, 'c')}};
    const nessodb::storage::Row replacement{{std::string(2000, 'd')}};

    {
        nessodb::storage::BufferPool buffer_pool(*database, 1);
        auto table_heap = nessodb::storage::TableHeap::create(buffer_pool, schema);
        if (!table_heap) {
            expect(false, "delete test table heap is created");
            return;
        }

        const auto first_id = table_heap->insert(first);
        const auto second_id = table_heap->insert(second);
        const auto third_id = table_heap->insert(third);
        expect(first_id && second_id && third_id &&
                   third_id->page_id == nessodb::common::PageId{2},
               "delete test rows span multiple pages");
        if (!first_id || !second_id || !third_id) {
            return;
        }

        expect(table_heap->erase(*second_id).has_value(),
               "row is deleted by record identifier");
        const auto repeated = table_heap->erase(*second_id);
        expect(!repeated &&
                   std::holds_alternative<nessodb::storage::SlottedPageError>(
                       repeated.error()) &&
                   std::get<nessodb::storage::SlottedPageError>(
                       repeated.error()) ==
                       nessodb::storage::SlottedPageError::deleted_slot,
               "deleting the same record twice is rejected");

        const auto rows = table_heap->scan();
        expect(rows && rows->size() == 2 &&
                   (*rows)[0].values == first.values &&
                   (*rows)[1].values == third.values,
               "scan skips deleted records across pages");
        const auto records = table_heap->scan_records();
        expect(records && records->size() == 2 &&
                   (*records)[0].record_id == *first_id &&
                   (*records)[1].record_id == *third_id,
               "record scan excludes deleted identifiers");

        expect(table_heap->restore(*second_id, second).has_value(),
               "table heap restores a deleted row");
        const auto restored = table_heap->scan_records();
        expect(restored && restored->size() == 3 &&
                   (*restored)[1].record_id == *second_id &&
                   (*restored)[1].row.values == second.values,
               "restoration preserves the record identifier and contents");
        const auto occupied = table_heap->restore(*second_id, second);
        expect(!occupied &&
                   std::holds_alternative<nessodb::storage::SlottedPageError>(
                       occupied.error()) &&
                   std::get<nessodb::storage::SlottedPageError>(
                       occupied.error()) ==
                       nessodb::storage::SlottedPageError::occupied_slot,
               "table heap rejects restoration over an occupied record");
        expect(table_heap->erase(*second_id).has_value(),
               "restored table row can be deleted again");

        expect(table_heap->erase(*third_id).has_value(),
               "tail-page row is deleted");
        const auto replacement_id = table_heap->insert(replacement);
        expect(replacement_id == third_id,
               "table heap reuses a deleted tail slot");

        const auto invalid = table_heap->erase(nessodb::storage::RecordId{});
        expect(!invalid &&
                   std::holds_alternative<nessodb::storage::TableHeapErrorCode>(
                       invalid.error()) &&
                   std::get<nessodb::storage::TableHeapErrorCode>(
                       invalid.error()) ==
                       nessodb::storage::TableHeapErrorCode::invalid_record_id,
               "invalid record identifier is rejected");
        const auto foreign = table_heap->erase(nessodb::storage::RecordId{
            nessodb::common::PageId{3}, nessodb::storage::SlotId{0}});
        expect(!foreign &&
                   std::holds_alternative<nessodb::storage::TableHeapErrorCode>(
                       foreign.error()) &&
                   std::get<nessodb::storage::TableHeapErrorCode>(
                       foreign.error()) ==
                       nessodb::storage::TableHeapErrorCode::record_page_not_found,
               "record page outside the table heap is rejected");
        expect(buffer_pool.flush().has_value(),
               "deleted table heap is flushed");
    }

    nessodb::storage::BufferPool buffer_pool(*database, 1);
    auto reopened = nessodb::storage::TableHeap::open(
        buffer_pool, schema, nessodb::common::PageId{1});
    expect(reopened.has_value(), "deleted table heap can be reopened");
    if (!reopened) {
        return;
    }
    const auto rows = reopened->scan();
    expect(rows && rows->size() == 2 &&
               (*rows)[0].values == first.values &&
               (*rows)[1].values == replacement.values,
           "deletion and slot reuse survive reopening");
}

void test_update_rows() {
    TemporaryDirectory directory;
    auto database = nessodb::storage::DatabaseFile::create(
        directory.path() / "update.mdb");
    if (!database) {
        expect(false, "update test database is created");
        return;
    }
    const nessodb::catalog::TableSchema schema{
        "documents",
        {{"body", nessodb::types::LogicalType::text}},
    };
    const nessodb::storage::Row first{{std::string(1000, 'a')}};
    const nessodb::storage::Row second{{std::string(1000, 'b')}};
    const nessodb::storage::Row updated_first{{std::string(1500, 'c')}};

    nessodb::storage::RecordId first_id;
    nessodb::storage::RecordId second_id;
    {
        nessodb::storage::BufferPool buffer_pool(*database, 1);
        auto table_heap = nessodb::storage::TableHeap::create(buffer_pool, schema);
        if (!table_heap) {
            expect(false, "update test table heap is created");
            return;
        }

        const auto inserted_first = table_heap->insert(first);
        const auto inserted_second = table_heap->insert(second);
        if (!inserted_first || !inserted_second) {
            expect(false, "update test rows are inserted");
            return;
        }
        first_id = *inserted_first;
        second_id = *inserted_second;

        expect(table_heap->update(first_id, updated_first).has_value(),
               "row is updated by record identifier");
        const auto records = table_heap->scan_records();
        expect(records && records->size() == 2 &&
                   (*records)[0].record_id == first_id &&
                   (*records)[0].row.values == updated_first.values &&
                   (*records)[1].record_id == second_id &&
                   (*records)[1].row.values == second.values,
               "table heap update preserves identifiers and surrounding rows");

        const nessodb::storage::Row oversized{{std::string(5000, 'x')}};
        const auto rejected = table_heap->update(first_id, oversized);
        expect(!rejected &&
                   std::holds_alternative<nessodb::storage::TableHeapErrorCode>(
                       rejected.error()) &&
                   std::get<nessodb::storage::TableHeapErrorCode>(
                       rejected.error()) ==
                       nessodb::storage::TableHeapErrorCode::record_too_large,
               "oversized replacement is rejected");

        const auto invalid =
            table_heap->update(nessodb::storage::RecordId{}, first);
        expect(!invalid &&
                   std::holds_alternative<nessodb::storage::TableHeapErrorCode>(
                       invalid.error()) &&
                   std::get<nessodb::storage::TableHeapErrorCode>(invalid.error()) ==
                       nessodb::storage::TableHeapErrorCode::invalid_record_id,
               "update rejects an invalid record identifier");

        const auto foreign = table_heap->update(nessodb::storage::RecordId{
                                                    nessodb::common::PageId{2},
                                                    nessodb::storage::SlotId{0}},
                                                first);
        expect(!foreign &&
                   std::holds_alternative<nessodb::storage::TableHeapErrorCode>(
                       foreign.error()) &&
                   std::get<nessodb::storage::TableHeapErrorCode>(foreign.error()) ==
                       nessodb::storage::TableHeapErrorCode::record_page_not_found,
               "update rejects a page outside the table heap");
        expect(buffer_pool.flush().has_value(), "updated table heap is flushed");
    }

    nessodb::storage::BufferPool buffer_pool(*database, 1);
    auto reopened = nessodb::storage::TableHeap::open(
        buffer_pool, schema, nessodb::common::PageId{1});
    expect(reopened.has_value(), "updated table heap can be reopened");
    if (!reopened) {
        return;
    }
    const auto records = reopened->scan_records();
    expect(records && records->size() == 2 &&
               (*records)[0].record_id == first_id &&
               (*records)[0].row.values == updated_first.values &&
               (*records)[1].record_id == second_id,
           "updated row and its identifier survive reopening");
}

}  // namespace

int main() {
    test_insert_rows();
    test_grow_table_heap();
    test_reopen_table_heap();
    test_scan_table_heap();
    test_delete_rows();
    test_update_rows();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
