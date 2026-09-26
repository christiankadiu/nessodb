#include "storage/catalog/catalog_store.hpp"

#include "catalog/schema.hpp"
#include "common/page_id.hpp"
#include "common/table_id.hpp"
#include "storage/buffer/buffer_pool.hpp"
#include "storage/catalog/catalog_record.hpp"
#include "storage/io/database_file.hpp"
#include "types/logical_type.hpp"

#include <chrono>
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
                ("nessodb-catalog-store-test-" + std::to_string(suffix));
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

void test_create_and_reopen_catalog_store() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "catalog.mdb";

    {
        auto database = nessodb::storage::DatabaseFile::create(path);
        if (!database) {
            expect(false, "catalog store database is created");
            return;
        }
        nessodb::storage::BufferPool buffer_pool(*database, 1);
        auto catalog = nessodb::storage::CatalogStore::create(*database, buffer_pool);
        expect(catalog && catalog->first_page_id() == nessodb::common::PageId{1},
               "catalog store creates its root heap page");
        expect(database->header().catalog_root == nessodb::common::PageId{1},
               "catalog store records its root in the database header");
        expect(!nessodb::storage::CatalogStore::create(*database, buffer_pool),
               "catalog store cannot be created twice");
        if (!catalog) {
            return;
        }

        auto table_page = buffer_pool.allocate_heap_page();
        expect(table_page.has_value(), "user table heap page is allocated");
        if (!table_page) {
            return;
        }
        const auto first_page_id = table_page->page_id;
        table_page->page.reset();

        const nessodb::catalog::TableSchema schema{
            "Users",
            {
                {"id", nessodb::types::LogicalType::integer},
                {"name", nessodb::types::LogicalType::text},
            },
            nessodb::common::TableId{7},
        };
        expect(catalog->add_table(schema, first_page_id).has_value(),
               "table metadata is added to catalog store");

        auto duplicate_id = schema;
        duplicate_id.name = "customers";
        expect(!catalog->add_table(duplicate_id, first_page_id),
               "duplicate table identifier is rejected");
        auto duplicate_name = schema;
        duplicate_name.id = nessodb::common::TableId{8};
        duplicate_name.name = "users";
        expect(!catalog->add_table(duplicate_name, first_page_id),
               "duplicate table name is rejected case-insensitively");
    }

    auto database = nessodb::storage::DatabaseFile::open(path);
    if (!database) {
        expect(false, "catalog store database is reopened");
        return;
    }
    nessodb::storage::BufferPool buffer_pool(*database, 1);
    const auto catalog = nessodb::storage::CatalogStore::open(*database, buffer_pool);
    expect(catalog && catalog->first_page_id() == nessodb::common::PageId{1},
           "catalog store reopens from the persisted root");
    if (!catalog) {
        return;
    }
    const auto tables = catalog->load_tables();
    expect(tables && tables->size() == 1,
           "stored table metadata is loaded after reopening");
    if (tables && tables->size() == 1) {
        const auto& table = tables->front();
        expect(table.schema.id == nessodb::common::TableId{7} &&
                   table.schema.name == "Users" &&
                   table.first_page_id == nessodb::common::PageId{2},
               "loaded table identity and heap root are preserved");
        expect(table.schema.columns.size() == 2 &&
                   table.schema.columns[0].name == "id" &&
                   table.schema.columns[0].type ==
                       nessodb::types::LogicalType::integer &&
                   table.schema.columns[1].name == "name" &&
                   table.schema.columns[1].type ==
                       nessodb::types::LogicalType::text,
               "loaded columns preserve ordinal, name, and type");
    }

    auto catalog_heap = nessodb::storage::TableHeap::open(
        buffer_pool, nessodb::storage::catalog_record_schema(),
        nessodb::common::PageId{1});
    expect(catalog_heap.has_value(), "catalog heap is reopened for inspection");
    if (!catalog_heap) {
        return;
    }
    const auto rows = catalog_heap->scan();
    expect(rows && rows->size() == 3,
           "column records and table completion record are persisted");
    if (rows && rows->size() == 3) {
        expect(std::holds_alternative<nessodb::storage::CatalogColumnRecord>(
                   *nessodb::storage::decode_catalog_record((*rows)[0])) &&
                   std::holds_alternative<nessodb::storage::CatalogColumnRecord>(
                       *nessodb::storage::decode_catalog_record((*rows)[1])) &&
                   std::holds_alternative<nessodb::storage::CatalogTableRecord>(
                       *nessodb::storage::decode_catalog_record((*rows)[2])),
               "table completion record follows its column records");
    }

    const auto orphan = nessodb::storage::encode_catalog_record(
        nessodb::storage::CatalogColumnRecord{
            nessodb::common::TableId{99}, 0,
            nessodb::catalog::ColumnSchema{
                "orphan", nessodb::types::LogicalType::integer}});
    expect(orphan && catalog_heap->insert(*orphan).has_value() &&
               buffer_pool.flush().has_value(),
           "uncommitted column record is persisted for recovery test");
    const auto recovered = catalog->load_tables();
    expect(recovered && recovered->size() == 1,
           "column records without a table marker are ignored");
}

void test_catalog_records() {
    const nessodb::storage::CatalogTableRecord table{
        nessodb::common::TableId{7}, "users", nessodb::common::PageId{12}, 2};
    const auto encoded_table = nessodb::storage::encode_catalog_record(table);
    expect(encoded_table.has_value(), "table catalog record is encoded");
    if (!encoded_table) {
        return;
    }
    const auto decoded_table =
        nessodb::storage::decode_catalog_record(*encoded_table);
    const auto* stored_table =
        decoded_table
            ? std::get_if<nessodb::storage::CatalogTableRecord>(&*decoded_table)
            : nullptr;
    expect(stored_table && stored_table->table_id == table.table_id &&
               stored_table->name == table.name &&
               stored_table->first_page_id == table.first_page_id &&
               stored_table->column_count == table.column_count,
           "table catalog record round-trips");

    const nessodb::storage::CatalogColumnRecord column{
        nessodb::common::TableId{7}, 1,
        nessodb::catalog::ColumnSchema{
            "name", nessodb::types::LogicalType::text, true}};
    const auto encoded_column = nessodb::storage::encode_catalog_record(column);
    expect(encoded_column.has_value(), "column catalog record is encoded");
    if (encoded_column) {
        const auto decoded = nessodb::storage::decode_catalog_record(*encoded_column);
        const auto* stored = decoded
                                 ? std::get_if<nessodb::storage::CatalogColumnRecord>(
                                       &*decoded)
                                 : nullptr;
        expect(stored && stored->table_id == column.table_id &&
                   stored->ordinal == column.ordinal &&
                   stored->column.name == column.column.name &&
                   stored->column.type == column.column.type &&
                   stored->column.primary_key,
               "column catalog record round-trips");
    }

    const nessodb::storage::CatalogIndexRecord index{
        nessodb::common::TableId{7}, 1, "users_primary_key",
        nessodb::common::PageId{13}, true, true};
    const auto encoded_index = nessodb::storage::encode_catalog_record(index);
    const auto decoded_index = encoded_index
                                   ? nessodb::storage::decode_catalog_record(
                                         *encoded_index)
                                   : std::expected<
                                         nessodb::storage::CatalogRecord,
                                         nessodb::storage::CatalogRecordError>{
                                         std::unexpected(
                                             nessodb::storage::CatalogRecordError::
                                                 invalid_field_count)};
    const auto* stored_index = decoded_index
        ? std::get_if<nessodb::storage::CatalogIndexRecord>(&*decoded_index)
        : nullptr;
    expect(stored_index && stored_index->table_id == index.table_id &&
               stored_index->ordinal == index.ordinal &&
               stored_index->name == index.name &&
               stored_index->root_page_id == index.root_page_id &&
               stored_index->unique && stored_index->primary_key,
           "index catalog record round-trips");

    auto unsupported = *encoded_table;
    unsupported.values[0] = std::int64_t{3};
    const auto rejected = nessodb::storage::decode_catalog_record(unsupported);
    expect(!rejected &&
               rejected.error() ==
                   nessodb::storage::CatalogRecordError::unsupported_format_version,
           "unsupported catalog record version is rejected");
}

}  // namespace

int main() {
    test_catalog_records();
    test_create_and_reopen_catalog_store();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
