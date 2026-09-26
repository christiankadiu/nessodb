#include "catalog/catalog.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

using nessodb::catalog::Catalog;
using nessodb::catalog::CatalogError;
using nessodb::catalog::ColumnSchema;
using nessodb::catalog::TableSchema;
using nessodb::types::LogicalType;

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

void test_create_and_find_table() {
    Catalog catalog;
    const auto created = catalog.create_table(TableSchema{
        "Users",
        {ColumnSchema{"id", LogicalType::integer}, ColumnSchema{"name", LogicalType::text}},
    });

    expect(created.has_value(), "table is created");

    const TableSchema* table = catalog.find_table("users");
    expect(table != nullptr, "table lookup is case-insensitive");
    expect(created && *created == table, "creation returns the stored table schema");
    if (table != nullptr) {
        expect(table->name == "Users", "original table name is preserved");
        expect(table->columns.size() == 2, "column schema is preserved");
        expect(table->id.is_valid(), "table receives a valid identifier");
    }

    const auto another_table = catalog.create_table(TableSchema{"orders", {}});
    expect(another_table.has_value(), "another table is created");
    if (table != nullptr) {
        expect(table->name == "Users", "table reference remains valid after insertion");
        const TableSchema* orders = catalog.find_table("orders");
        expect(orders != nullptr, "another table can be found");
        if (orders != nullptr) {
            expect(orders->id != table->id, "different tables receive different identifiers");
        }
    }
    expect(catalog.find_table("missing") == nullptr, "unknown table is not found");
}

void test_duplicate_table() {
    Catalog catalog;
    const auto first = catalog.create_table(TableSchema{"users", {}});
    const auto duplicate = catalog.create_table(TableSchema{"USERS", {}});

    expect(first.has_value(), "first table is created");
    expect(!duplicate, "duplicate table is rejected");
    if (!duplicate) {
        expect(duplicate.error() == CatalogError::table_already_exists,
               "duplicate table has expected error");
    }
}

void test_restore_tables() {
    Catalog catalog;
    const auto restored = catalog.restore_table(
        TableSchema{"users", {}, nessodb::common::TableId{7}});
    expect(restored && (*restored)->id == nessodb::common::TableId{7},
           "table is restored with its persisted identifier");
    expect(catalog.find_table(nessodb::common::TableId{7}) == *restored,
           "restored table can be found by identifier");

    const auto created = catalog.create_table(TableSchema{"orders", {}});
    expect(created && (*created)->id == nessodb::common::TableId{8},
           "new identifier follows highest restored identifier");

    const auto duplicate_id = catalog.restore_table(
        TableSchema{"customers", {}, nessodb::common::TableId{7}});
    expect(!duplicate_id &&
               duplicate_id.error() == CatalogError::table_id_already_exists,
           "duplicate restored identifier is rejected");
    const auto invalid = catalog.restore_table(TableSchema{"invalid", {}});
    expect(!invalid && invalid.error() == CatalogError::invalid_table_id,
           "invalid restored identifier is rejected");

    Catalog exhausted;
    const auto maximum = exhausted.restore_table(TableSchema{
        "last", {},
        nessodb::common::TableId{std::numeric_limits<std::uint64_t>::max()}});
    expect(maximum.has_value(), "maximum table identifier can be restored");
    const auto no_next = exhausted.create_table(TableSchema{"next", {}});
    expect(!no_next && no_next.error() == CatalogError::table_id_exhausted,
           "identifier exhaustion is reported without wrapping to zero");
}

}  // namespace

int main() {
    test_create_and_find_table();
    test_duplicate_table();
    test_restore_tables();

    if (failures != 0) {
        std::cerr << failures << " catalog assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
