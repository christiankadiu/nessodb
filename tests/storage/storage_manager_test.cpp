#include "storage/storage_manager.hpp"

#include "catalog/schema.hpp"
#include "common/table_id.hpp"
#include "storage/access/table_heap.hpp"
#include "storage/access/row.hpp"
#include "storage/buffer/buffer_pool.hpp"
#include "storage/catalog/catalog_store.hpp"
#include "storage/io/database_file.hpp"
#include "types/logical_type.hpp"
#include "types/value.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

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
                ("nessodb-storage-manager-test-" + std::to_string(suffix));
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

void test_create_and_open_storage_manager() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "database.mdb";

    {
        auto manager = nessodb::storage::StorageManager::create(path, 3);
        expect(manager.has_value(), "storage manager creates a database");
        if (!manager) {
            return;
        }
        expect(manager->path() == path, "storage manager preserves database path");
        expect(manager->buffer_pool_capacity() == 3,
               "storage manager preserves buffer pool capacity");
        expect(manager->tables().empty(), "new storage manager has no user tables");

        const nessodb::catalog::TableSchema schema{
            "items",
            {{"name", nessodb::types::LogicalType::text}},
            nessodb::common::TableId{1},
        };
        const auto created_table = manager->create_table(schema);
        expect(created_table &&
                   created_table->first_page_id == nessodb::common::PageId{2},
               "storage manager creates a user table heap");
        expect(manager->tables().size() == 1,
               "created table is visible through storage manager metadata");
        expect(!manager->create_table(schema),
               "storage manager rejects duplicate table metadata");
        const auto inserted = manager->insert(
            schema.id, nessodb::storage::Row{{std::string{"widget"}}});
        expect(inserted && inserted->page_id == nessodb::common::PageId{2} &&
                   inserted->slot_id == nessodb::storage::SlotId{0},
               "storage manager inserts a row into the table heap");
        expect(!manager->insert(
                   nessodb::common::TableId{99},
                   nessodb::storage::Row{{std::string{"missing"}}}),
               "storage manager rejects insert into an unknown table");
        const auto rows = manager->scan(schema.id);
        expect(rows && rows->size() == 1 &&
                   std::get<std::string>(rows->front().values.front()) == "widget",
               "storage manager scans inserted rows");
        const auto records = manager->scan_records(schema.id);
        expect(inserted && records && records->size() == 1 &&
                   records->front().record_id == *inserted &&
                   std::get<std::string>(
                       records->front().row.values.front()) == "widget",
               "storage manager scans rows with record identifiers");
        expect(!manager->scan(nessodb::common::TableId{99}),
               "storage manager rejects scan of an unknown table");
        expect(!manager->scan_records(nessodb::common::TableId{99}),
               "storage manager rejects record scan of an unknown table");
    }

    const auto reopened = nessodb::storage::StorageManager::open(path, 2);
    expect(reopened.has_value(), "storage manager reopens a database");
    if (reopened) {
        expect(reopened->path() == path, "reopened manager preserves database path");
        expect(reopened->buffer_pool_capacity() == 2,
               "reopened manager uses requested buffer pool capacity");
        expect(reopened->tables().size() == 1 &&
                   reopened->tables().front().schema.name == "items",
               "created table survives storage manager reopening");
        const auto rows = reopened->scan(nessodb::common::TableId{1});
        expect(rows && rows->size() == 1 &&
                   std::get<std::string>(rows->front().values.front()) == "widget",
               "storage manager scans durable rows after reopening");
    }

    auto inspection_file = nessodb::storage::DatabaseFile::open(path);
    if (!inspection_file) {
        expect(false, "inserted row database is opened for inspection");
        return;
    }
    nessodb::storage::BufferPool inspection_pool(*inspection_file, 1);
    const nessodb::catalog::TableSchema stored_schema{
        "items",
        {{"name", nessodb::types::LogicalType::text}},
        nessodb::common::TableId{1},
    };
    auto stored_heap = nessodb::storage::TableHeap::open(
        inspection_pool, stored_schema, nessodb::common::PageId{2});
    if (!stored_heap) {
        expect(false, "inserted row table heap is reopened for inspection");
        return;
    }
    const auto rows = stored_heap->scan();
    expect(rows && rows->size() == 1 &&
               std::get<std::string>(rows->front().values.front()) == "widget",
           "inserted row is durable after storage manager reopening");

    expect(!nessodb::storage::StorageManager::create(
               directory.path() / "invalid.mdb", 0),
           "zero buffer pool capacity is rejected");
}

void test_load_stored_tables() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "stored-tables.mdb";
    {
        auto database = nessodb::storage::DatabaseFile::create(path);
        if (!database) {
            expect(false, "stored table database is created");
            return;
        }
        nessodb::storage::BufferPool buffer_pool(*database, 2);
        auto catalog_store =
            nessodb::storage::CatalogStore::create(*database, buffer_pool);
        const nessodb::catalog::TableSchema schema{
            "users",
            {{"id", nessodb::types::LogicalType::integer}},
            nessodb::common::TableId{4},
        };
        auto table_heap = nessodb::storage::TableHeap::create(buffer_pool, schema);
        if (!catalog_store || !table_heap) {
            expect(false, "stored table structures are created");
            return;
        }
        expect(catalog_store->add_table(schema, table_heap->first_page_id()).has_value(),
               "stored table metadata is persisted");
    }

    const auto manager = nessodb::storage::StorageManager::open(path, 1);
    expect(manager && manager->tables().size() == 1,
           "storage manager loads persisted tables");
    if (manager && manager->tables().size() == 1) {
        const auto& table = manager->tables().front();
        expect(table.schema.id == nessodb::common::TableId{4} &&
                   table.schema.name == "users" &&
                   table.schema.columns.size() == 1 &&
                   table.first_page_id == nessodb::common::PageId{2},
               "storage manager preserves loaded table metadata");
    }
}

void test_delete_rows() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "delete.mdb";
    const nessodb::catalog::TableSchema schema{
        "items",
        {{"name", nessodb::types::LogicalType::text}},
        nessodb::common::TableId{1},
    };

    {
        auto manager = nessodb::storage::StorageManager::create(path, 2);
        if (!manager || !manager->create_table(schema)) {
            expect(false, "delete test storage is created");
            return;
        }
        const auto first = manager->insert(
            schema.id, nessodb::storage::Row{{std::string{"first"}}});
        const auto second = manager->insert(
            schema.id, nessodb::storage::Row{{std::string{"second"}}});
        expect(first && second, "delete test rows are inserted");
        if (!first || !second) {
            return;
        }

        expect(manager->erase(schema.id, *first).has_value(),
               "storage manager deletes a row");
        const auto rows = manager->scan(schema.id);
        expect(rows && rows->size() == 1 &&
                   std::get<std::string>(rows->front().values.front()) ==
                       "second",
               "storage manager scan excludes deleted row");
        const auto records = manager->scan_records(schema.id);
        expect(records && records->size() == 1 &&
                   records->front().record_id == *second,
               "storage manager record scan excludes deleted identifier");
        const nessodb::storage::Row restored_row{{std::string{"first"}}};
        expect(manager->restore(schema.id, *first, restored_row).has_value(),
               "storage manager restores a deleted row");
        const auto restored = manager->scan_records(schema.id);
        expect(restored && restored->size() == 2 &&
                   restored->front().record_id == *first &&
                   restored->front().row.values == restored_row.values,
               "storage manager restoration preserves the record identifier");
        expect(manager->erase(schema.id, *first).has_value(),
               "restored storage row can be deleted again");
        expect(!manager->erase(nessodb::common::TableId{99}, *second),
               "storage manager rejects deletion from an unknown table");
    }

    const auto reopened = nessodb::storage::StorageManager::open(path, 1);
    expect(reopened.has_value(), "deleted storage manager can be reopened");
    if (!reopened) {
        return;
    }
    const auto rows = reopened->scan(schema.id);
    expect(rows && rows->size() == 1 &&
               std::get<std::string>(rows->front().values.front()) == "second",
           "storage manager deletion survives reopening");
}

void test_update_rows() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "update.mdb";
    const nessodb::catalog::TableSchema schema{
        "items",
        {{"name", nessodb::types::LogicalType::text}},
        nessodb::common::TableId{1},
    };
    nessodb::storage::RecordId record_id;

    {
        auto manager = nessodb::storage::StorageManager::create(path, 2);
        if (!manager || !manager->create_table(schema)) {
            expect(false, "update test storage is created");
            return;
        }
        const auto inserted = manager->insert(
            schema.id, nessodb::storage::Row{{std::string{"before"}}});
        if (!inserted) {
            expect(false, "update test row is inserted");
            return;
        }
        record_id = *inserted;

        const nessodb::storage::Row replacement{
            {std::string{"updated value"}}};
        expect(manager->update(schema.id, record_id, replacement).has_value(),
               "storage manager updates a row");
        const auto records = manager->scan_records(schema.id);
        expect(records && records->size() == 1 &&
                   records->front().record_id == record_id &&
                   records->front().row.values == replacement.values,
               "storage manager preserves the updated record identifier");
        expect(!manager->update(nessodb::common::TableId{99}, record_id,
                                replacement),
               "storage manager rejects update of an unknown table");
    }

    const auto reopened = nessodb::storage::StorageManager::open(path, 1);
    expect(reopened.has_value(), "updated storage manager can be reopened");
    if (!reopened) {
        return;
    }
    const auto records = reopened->scan_records(schema.id);
    expect(records && records->size() == 1 &&
               records->front().record_id == record_id &&
               std::get<std::string>(records->front().row.values.front()) ==
                   "updated value",
           "storage manager update is durable");
}

void test_primary_key_index() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "primary-key.mdb";
    const nessodb::catalog::TableSchema schema{
        "users",
        {{"id", nessodb::types::LogicalType::integer, true},
         {"name", nessodb::types::LogicalType::text}},
        nessodb::common::TableId{1},
    };
    std::vector<nessodb::storage::RecordId> record_ids;

    {
        auto manager = nessodb::storage::StorageManager::create(path, 8);
        if (!manager) {
            expect(false, "primary key storage manager is created");
            return;
        }
        const auto created = manager->create_table(schema);
        expect(created && created->indexes.size() == 1 &&
                   created->indexes.front().primary_key &&
                   created->indexes.front().unique,
               "primary key creation persists unique index metadata");
        if (!created) {
            return;
        }
        const auto initial_root = created->indexes.front().root_page_id;

        record_ids.reserve(300);
        for (std::int64_t id = 0; id < 300; ++id) {
            auto inserted = manager->insert(
                schema.id,
                nessodb::storage::Row{{id, "user-" + std::to_string(id)}});
            if (!inserted) {
                expect(false, "primary key rows are inserted");
                return;
            }
            record_ids.push_back(*inserted);
        }
        expect(manager->tables().front().indexes.front().root_page_id !=
                   initial_root,
               "primary key root changes after a B+ tree split");
        const auto found = manager->lookup_primary_key(
            schema.id, nessodb::types::Value{std::int64_t{299}});
        expect(found && found->size() == 1 &&
                   std::get<std::string>(found->front().values[1]) ==
                       "user-299",
               "primary key lookup finds the indexed row");

        const auto duplicate = manager->insert(
            schema.id,
            nessodb::storage::Row{{std::int64_t{299}, "duplicate"}});
        expect(!duplicate &&
                   std::get_if<nessodb::storage::StorageManagerErrorCode>(
                       &duplicate.error()) != nullptr &&
                   std::get<nessodb::storage::StorageManagerErrorCode>(
                       duplicate.error()) ==
                       nessodb::storage::StorageManagerErrorCode::
                           unique_constraint_violation,
               "primary key rejects duplicate values");

        const auto null_key = manager->insert(
            schema.id,
            nessodb::storage::Row{{nessodb::types::NullValue{}, "null"}});
        expect(!null_key &&
                   std::get_if<nessodb::storage::StorageManagerErrorCode>(
                       &null_key.error()) != nullptr &&
                   std::get<nessodb::storage::StorageManagerErrorCode>(
                       null_key.error()) ==
                       nessodb::storage::StorageManagerErrorCode::
                           primary_key_null,
               "primary key rejects NULL values");

        const nessodb::storage::Row updated{
            {std::int64_t{1000}, std::string{"renamed"}}};
        expect(manager->update(schema.id, record_ids.back(), updated)
                   .has_value(),
               "primary key index is maintained on update");
        expect(manager->erase(schema.id, record_ids.front()).has_value(),
               "primary key index is maintained on delete");
    }

    auto reopened = nessodb::storage::StorageManager::open(path, 4);
    expect(reopened && reopened->tables().size() == 1 &&
               reopened->tables().front().indexes.size() == 1,
           "primary key index metadata survives reopening");
    if (!reopened) {
        return;
    }
    const auto updated = reopened->lookup_primary_key(
        schema.id, nessodb::types::Value{std::int64_t{1000}});
    const auto erased = reopened->lookup_primary_key(
        schema.id, nessodb::types::Value{std::int64_t{0}});
    expect(updated && updated->size() == 1 &&
               std::get<std::string>(updated->front().values[1]) == "renamed",
           "reopened primary key index finds an updated key");
    expect(erased && erased->empty(),
           "reopened primary key index excludes a deleted key");
}

}  // namespace

int main() {
    test_create_and_open_storage_manager();
    test_load_stored_tables();
    test_delete_rows();
    test_update_rows();
    test_primary_key_index();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
