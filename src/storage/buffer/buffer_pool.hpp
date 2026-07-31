#pragma once

#include "common/page_id.hpp"
#include "storage/io/database_file.hpp"
#include "storage/page/page.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <list>
#include <memory>
#include <unordered_map>
#include <variant>

namespace minidb::storage {

enum class BufferPoolErrorCode {
    capacity_exceeded,
    all_pages_pinned,
};

using BufferPoolError = std::variant<DatabaseFileError, BufferPoolErrorCode>;
using ReadPageHandle = std::shared_ptr<const PageBuffer>;
using WritePageHandle = std::shared_ptr<PageBuffer>;

struct AllocatedHeapPage {
    common::PageId page_id;
    WritePageHandle page;
};

class BufferPool {
public:
    BufferPool(DatabaseFile& database_file, std::size_t capacity);

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    BufferPool(BufferPool&&) = delete;
    BufferPool& operator=(BufferPool&&) = delete;
    ~BufferPool() = default;

    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::expected<AllocatedHeapPage, BufferPoolError> allocate_heap_page();
    [[nodiscard]] std::expected<ReadPageHandle, BufferPoolError> fetch_heap_page(
        common::PageId page_id);
    [[nodiscard]] std::expected<WritePageHandle, BufferPoolError> fetch_heap_page_for_write(
        common::PageId page_id);
    [[nodiscard]] std::expected<void, BufferPoolError> flush();

private:
    struct Frame {
        std::shared_ptr<PageBuffer> page;
        std::list<std::uint64_t>::iterator recency;
        bool dirty{};
    };

    void touch(Frame& frame) noexcept;
    [[nodiscard]] std::expected<void, BufferPoolError> ensure_available_frame();
    [[nodiscard]] std::expected<bool, DatabaseFileError> evict_page();

    DatabaseFile& database_file_;
    std::size_t capacity_;
    std::list<std::uint64_t> recency_;
    std::unordered_map<std::uint64_t, Frame> pages_;
};

}  // namespace minidb::storage
