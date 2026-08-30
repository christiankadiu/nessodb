#pragma once

#include "common/table_id.hpp"
#include "storage/access/in_memory_heap.hpp"
#include "storage/access/record_id.hpp"
#include "storage/access/row.hpp"
#include "storage/storage_manager.hpp"

#include <cstddef>
#include <expected>
#include <variant>
#include <vector>

namespace minidb::storage {

enum class UndoErrorCode {
    row_identifier_mismatch,
};

using UndoError =
    std::variant<HeapError, StorageManagerError, UndoErrorCode>;

class UndoLog {
public:
    UndoLog() = default;

    UndoLog(const UndoLog&) = delete;
    UndoLog& operator=(const UndoLog&) = delete;
    UndoLog(UndoLog&&) noexcept = default;
    UndoLog& operator=(UndoLog&&) noexcept = default;

    void record_insert(common::TableId table_id, InMemoryRowId row_id);
    void record_insert(common::TableId table_id, RecordId record_id);
    void record_delete(common::TableId table_id, InMemoryRowId row_id,
                       Row row);
    void record_delete(common::TableId table_id, RecordId record_id, Row row);
    void record_update(common::TableId table_id, InMemoryRowId row_id,
                       Row before);
    void record_update(common::TableId table_id, RecordId record_id,
                       Row before);

    [[nodiscard]] std::expected<void, UndoError> rollback(
        InMemoryHeap& heap);
    [[nodiscard]] std::expected<void, UndoError> rollback(
        StorageManager& storage);
    void clear() noexcept;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    using RowIdentifier = std::variant<InMemoryRowId, RecordId>;

    struct InsertUndo {
        common::TableId table_id;
        RowIdentifier row_id;
    };

    struct DeleteUndo {
        common::TableId table_id;
        RowIdentifier row_id;
        Row row;
    };

    struct UpdateUndo {
        common::TableId table_id;
        RowIdentifier row_id;
        Row before;
    };

    using UndoRecord = std::variant<InsertUndo, DeleteUndo, UpdateUndo>;

    std::vector<UndoRecord> records_;
};

}  // namespace minidb::storage
