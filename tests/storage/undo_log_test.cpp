#include "storage/undo_log.hpp"

#include "catalog/schema.hpp"
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

using nessodb::common::TableId;
using nessodb::storage::InMemoryHeap;
using nessodb::storage::RecordId;
using nessodb::storage::Row;
using nessodb::storage::StorageManager;
using nessodb::storage::UndoErrorCode;
using nessodb::storage::UndoLog;

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
        const auto suffix =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("nessodb-undo-log-test-" + std::to_string(suffix));
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

void test_in_memory_undo_runs_in_reverse_order() {
    const TableId table_id{1};
    const Row original{{std::int64_t{1}, std::string{"original"}}};
    const Row updated{{std::int64_t{2}, std::string{"updated"}}};
    const Row transient{{std::int64_t{3}, std::string{"transient"}}};
    InMemoryHeap heap;
    expect(heap.create_table(table_id).has_value(),
           "in-memory undo table is created");
    const auto row_id = heap.insert(table_id, original);
    expect(row_id.has_value(), "original in-memory row is inserted");
    if (!row_id) {
        return;
    }

    UndoLog undo;
    undo.record_update(table_id, *row_id, original);
    expect(heap.update(table_id, *row_id, updated).has_value(),
           "row is updated before rollback");
    undo.record_delete(table_id, *row_id, updated);
    expect(heap.erase(table_id, *row_id).has_value(),
           "updated row is deleted before rollback");
    const auto transient_id = heap.insert(table_id, transient);
    expect(transient_id == row_id, "deleted row identifier is reused");
    if (!transient_id) {
        return;
    }
    undo.record_insert(table_id, *transient_id);

    expect(undo.size() == 3 && undo.rollback(heap).has_value(),
           "in-memory undo applies all records");
    const auto records = heap.scan_records(table_id);
    expect(undo.empty() && records && records->size() == 1 &&
               records->front().row_id == *row_id &&
               records->front().row.values == original.values,
           "reverse undo restores the original row and identifier");
}

void test_persistent_undo_is_durable() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "undo.mdb";
    const nessodb::catalog::TableSchema schema{
        "items",
        {{"value", nessodb::types::LogicalType::text}},
        TableId{1},
    };
    const Row original{{std::string{"original"}}};
    const Row updated{{std::string{"updated"}}};
    const Row transient{{std::string{"transient"}}};
    RecordId row_id;

    {
        auto storage = StorageManager::create(path, 2);
        expect(storage.has_value() && storage->create_table(schema).has_value(),
               "persistent undo storage is created");
        if (!storage) {
            return;
        }
        const auto inserted = storage->insert(schema.id, original);
        expect(inserted.has_value(), "original persistent row is inserted");
        if (!inserted) {
            return;
        }
        row_id = *inserted;

        UndoLog undo;
        undo.record_update(schema.id, row_id, original);
        expect(storage->update(schema.id, row_id, updated).has_value(),
               "persistent row is updated before rollback");
        undo.record_delete(schema.id, row_id, updated);
        expect(storage->erase(schema.id, row_id).has_value(),
               "persistent row is deleted before rollback");
        const auto transient_id = storage->insert(schema.id, transient);
        expect(transient_id == row_id,
               "persistent storage reuses the deleted record identifier");
        if (!transient_id) {
            return;
        }
        undo.record_insert(schema.id, *transient_id);

        expect(undo.rollback(*storage).has_value() && undo.empty(),
               "persistent undo applies all records");
    }

    auto reopened = StorageManager::open(path, 1);
    expect(reopened.has_value(), "rolled-back storage reopens");
    if (!reopened) {
        return;
    }
    const auto records = reopened->scan_records(schema.id);
    expect(records && records->size() == 1 &&
               records->front().record_id == row_id &&
               records->front().row.values == original.values,
           "persistent undo survives reopening");
}

void test_undo_rejects_the_wrong_storage_backend() {
    const TableId table_id{1};
    InMemoryHeap heap;
    expect(heap.create_table(table_id).has_value(),
           "mismatch test table is created");

    UndoLog undo;
    undo.record_insert(table_id, RecordId{});
    const auto rolled_back = undo.rollback(heap);
    expect(!rolled_back && undo.size() == 1 &&
               std::holds_alternative<UndoErrorCode>(rolled_back.error()) &&
               std::get<UndoErrorCode>(rolled_back.error()) ==
                   UndoErrorCode::row_identifier_mismatch,
           "backend mismatch preserves the unapplied undo record");
    undo.clear();
    expect(undo.empty(), "committed undo records can be cleared");
}

}  // namespace

int main() {
    test_in_memory_undo_runs_in_reverse_order();
    test_persistent_undo_is_durable();
    test_undo_rejects_the_wrong_storage_backend();

    if (failures != 0) {
        std::cerr << failures << " undo log assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
