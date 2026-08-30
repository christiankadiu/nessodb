#include "storage/undo_log.hpp"

#include <type_traits>
#include <utility>

namespace minidb::storage {
namespace {

template <typename RowId, typename RowIdentifier>
std::expected<RowId, UndoError> require_row_id(
    const RowIdentifier& row_id) {
    const auto* typed = std::get_if<RowId>(&row_id);
    if (typed == nullptr) {
        return std::unexpected(UndoError{UndoErrorCode::row_identifier_mismatch});
    }
    return *typed;
}

}  // namespace

void UndoLog::record_insert(common::TableId table_id,
                            InMemoryRowId row_id) {
    records_.emplace_back(InsertUndo{table_id, row_id});
}

void UndoLog::record_insert(common::TableId table_id, RecordId record_id) {
    records_.emplace_back(InsertUndo{table_id, record_id});
}

void UndoLog::record_delete(common::TableId table_id, InMemoryRowId row_id,
                            Row row) {
    records_.emplace_back(
        DeleteUndo{table_id, row_id, std::move(row)});
}

void UndoLog::record_delete(common::TableId table_id, RecordId record_id,
                            Row row) {
    records_.emplace_back(
        DeleteUndo{table_id, record_id, std::move(row)});
}

void UndoLog::record_update(common::TableId table_id, InMemoryRowId row_id,
                            Row before) {
    records_.emplace_back(
        UpdateUndo{table_id, row_id, std::move(before)});
}

void UndoLog::record_update(common::TableId table_id, RecordId record_id,
                            Row before) {
    records_.emplace_back(
        UpdateUndo{table_id, record_id, std::move(before)});
}

std::expected<void, UndoError> UndoLog::rollback(InMemoryHeap& heap) {
    while (!records_.empty()) {
        const auto applied = std::visit(
            [&heap](const auto& record) -> std::expected<void, UndoError> {
                using Record = std::decay_t<decltype(record)>;
                auto row_id = require_row_id<InMemoryRowId>(record.row_id);
                if (!row_id) {
                    return std::unexpected(row_id.error());
                }

                if constexpr (std::is_same_v<Record, InsertUndo>) {
                    auto erased = heap.erase(record.table_id, *row_id);
                    if (!erased) {
                        return std::unexpected(UndoError{erased.error()});
                    }
                } else if constexpr (std::is_same_v<Record, DeleteUndo>) {
                    auto restored =
                        heap.restore(record.table_id, *row_id, record.row);
                    if (!restored) {
                        return std::unexpected(UndoError{restored.error()});
                    }
                } else {
                    auto restored =
                        heap.update(record.table_id, *row_id, record.before);
                    if (!restored) {
                        return std::unexpected(UndoError{restored.error()});
                    }
                }
                return {};
            },
            records_.back());
        if (!applied) {
            return std::unexpected(applied.error());
        }
        records_.pop_back();
    }
    return {};
}

std::expected<void, UndoError> UndoLog::rollback(StorageManager& storage) {
    while (!records_.empty()) {
        const auto applied = std::visit(
            [&storage](const auto& record) -> std::expected<void, UndoError> {
                using Record = std::decay_t<decltype(record)>;
                auto record_id = require_row_id<RecordId>(record.row_id);
                if (!record_id) {
                    return std::unexpected(record_id.error());
                }

                if constexpr (std::is_same_v<Record, InsertUndo>) {
                    auto erased = storage.erase(record.table_id, *record_id);
                    if (!erased) {
                        return std::unexpected(UndoError{erased.error()});
                    }
                } else if constexpr (std::is_same_v<Record, DeleteUndo>) {
                    auto restored = storage.restore(
                        record.table_id, *record_id, record.row);
                    if (!restored) {
                        return std::unexpected(UndoError{restored.error()});
                    }
                } else {
                    auto restored = storage.update(
                        record.table_id, *record_id, record.before);
                    if (!restored) {
                        return std::unexpected(UndoError{restored.error()});
                    }
                }
                return {};
            },
            records_.back());
        if (!applied) {
            return std::unexpected(applied.error());
        }
        records_.pop_back();
    }
    return {};
}

void UndoLog::clear() noexcept {
    records_.clear();
}

bool UndoLog::empty() const noexcept {
    return records_.empty();
}

std::size_t UndoLog::size() const noexcept {
    return records_.size();
}

}  // namespace minidb::storage
