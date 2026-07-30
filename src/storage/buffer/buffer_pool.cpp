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
        touch(cached->second);
        return ReadPageHandle{cached->second.page};
    }
    if (capacity_ == 0) {
        return std::unexpected(BufferPoolError{BufferPoolErrorCode::capacity_exceeded});
    }

    auto page = database_file_.read_heap_page(page_id);
    if (!page) {
        return std::unexpected(BufferPoolError{page.error()});
    }
    if (pages_.size() >= capacity_) {
        auto evicted = evict_page();
        if (!evicted) {
            return std::unexpected(BufferPoolError{evicted.error()});
        }
        if (!*evicted) {
            return std::unexpected(BufferPoolError{BufferPoolErrorCode::all_pages_pinned});
        }
    }

    auto stored_page = std::make_shared<PageBuffer>(std::move(*page));
    recency_.push_front(page_id.value);
    try {
        pages_.emplace(page_id.value, Frame{stored_page, recency_.begin()});
    } catch (...) {
        recency_.pop_front();
        throw;
    }
    return ReadPageHandle{std::move(stored_page)};
}

std::expected<WritePageHandle, BufferPoolError> BufferPool::fetch_heap_page_for_write(
    common::PageId page_id) {
    auto page = fetch_heap_page(page_id);
    if (!page) {
        return std::unexpected(page.error());
    }

    auto frame = pages_.find(page_id.value);
    frame->second.dirty = true;
    return std::const_pointer_cast<PageBuffer>(std::move(*page));
}

std::expected<void, BufferPoolError> BufferPool::flush() {
    for (auto& [page_id, frame] : pages_) {
        if (!frame.dirty) {
            continue;
        }
        auto written = database_file_.write_heap_page(common::PageId{page_id}, *frame.page);
        if (!written) {
            return std::unexpected(BufferPoolError{written.error()});
        }
        if (frame.page.use_count() == 1) {
            frame.dirty = false;
        }
    }
    return {};
}

void BufferPool::touch(Frame& frame) noexcept {
    recency_.splice(recency_.begin(), recency_, frame.recency);
    frame.recency = recency_.begin();
}

std::expected<bool, DatabaseFileError> BufferPool::evict_page() {
    auto candidate = recency_.end();
    while (candidate != recency_.begin()) {
        --candidate;
        auto frame = pages_.find(*candidate);
        if (frame->second.page.use_count() == 1) {
            if (frame->second.dirty) {
                auto written = database_file_.write_heap_page(
                    common::PageId{frame->first}, *frame->second.page);
                if (!written) {
                    return std::unexpected(written.error());
                }
            }
            pages_.erase(frame);
            recency_.erase(candidate);
            return true;
        }
    }
    return false;
}

}  // namespace minidb::storage
