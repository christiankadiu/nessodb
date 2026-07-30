#include "storage/buffer/buffer_pool.hpp"

#include <utility>

namespace minidb::storage {

BufferPool::BufferPool(DatabaseFile& database_file, std::size_t capacity)
    : database_file_(database_file), capacity_(capacity) {
    pages_.reserve(capacity);
}

std::size_t BufferPool::capacity() const noexcept {
    return capacity_;
}

std::size_t BufferPool::size() const noexcept {
    return pages_.size();
}

std::expected<ReadPageHandle, BufferPoolError> BufferPool::fetch_heap_page(
    common::PageId page_id) {
    const auto cached = pages_.find(page_id.value);
    if (cached != pages_.end()) {
        return ReadPageHandle{cached->second};
    }
    if (pages_.size() >= capacity_) {
        return std::unexpected(BufferPoolError{BufferPoolErrorCode::capacity_exceeded});
    }

    auto page = database_file_.read_heap_page(page_id);
    if (!page) {
        return std::unexpected(BufferPoolError{page.error()});
    }

    auto stored_page = std::make_shared<PageBuffer>(std::move(*page));
    pages_.emplace(page_id.value, stored_page);
    return ReadPageHandle{std::move(stored_page)};
}

}  // namespace minidb::storage
