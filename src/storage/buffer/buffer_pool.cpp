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

std::expected<AllocatedHeapPage, BufferPoolError> BufferPool::allocate_heap_page() {
    auto available = ensure_available_frame();
    if (!available) {
        return std::unexpected(available.error());
    }

    auto page_id = database_file_.allocate_heap_page();
    if (!page_id) {
        return std::unexpected(BufferPoolError{page_id.error()});
    }
    auto page = fetch_heap_page_for_write(*page_id);
    if (!page) {
        return std::unexpected(page.error());
    }
    return AllocatedHeapPage{*page_id, std::move(*page)};
}

std::expected<AllocatedIndexPage, BufferPoolError>
BufferPool::allocate_index_leaf_page() {
    auto available = ensure_available_frame();
    if (!available) {
        return std::unexpected(available.error());
    }
    auto page_id = database_file_.allocate_index_leaf_page();
    if (!page_id) {
        return std::unexpected(BufferPoolError{page_id.error()});
    }
    auto page = fetch_index_page_for_write(*page_id);
    if (!page) {
        return std::unexpected(page.error());
    }
    return AllocatedIndexPage{*page_id, std::move(*page)};
}

std::expected<AllocatedIndexPage, BufferPoolError>
BufferPool::allocate_index_internal_page(
    std::uint16_t level, common::PageId leftmost_child) {
    auto available = ensure_available_frame();
    if (!available) {
        return std::unexpected(available.error());
    }
    auto page_id = database_file_.allocate_index_internal_page(
        level, leftmost_child);
    if (!page_id) {
        return std::unexpected(BufferPoolError{page_id.error()});
    }
    auto page = fetch_index_page_for_write(*page_id);
    if (!page) {
        return std::unexpected(page.error());
    }
    return AllocatedIndexPage{*page_id, std::move(*page)};
}

std::expected<ReadPageHandle, BufferPoolError> BufferPool::fetch_page(
    common::PageId page_id) {
    const auto cached = pages_.find(page_id.value);
    if (cached != pages_.end()) {
        touch(cached->second);
        return ReadPageHandle{cached->second.page};
    }
    if (capacity_ == 0) {
        return std::unexpected(BufferPoolError{BufferPoolErrorCode::capacity_exceeded});
    }

    auto page = database_file_.read_page(page_id);
    if (!page) {
        return std::unexpected(BufferPoolError{page.error()});
    }
    auto available = ensure_available_frame();
    if (!available) {
        return std::unexpected(available.error());
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

std::expected<ReadPageHandle, BufferPoolError> BufferPool::fetch_heap_page(
    common::PageId page_id) {
    auto page = fetch_page(page_id);
    if (!page) {
        return std::unexpected(page.error());
    }
    PageBuffer copy = **page;
    auto decoded = SlottedPage::open(copy);
    if (!decoded) {
        return std::unexpected(BufferPoolError{
            DatabaseFileError{decoded.error()}});
    }
    return page;
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

std::expected<ReadPageHandle, BufferPoolError> BufferPool::fetch_index_page(
    common::PageId page_id) {
    auto page = fetch_page(page_id);
    if (!page) {
        return std::unexpected(page.error());
    }
    const auto header = decode_page_header(**page);
    if (!header) {
        return std::unexpected(BufferPoolError{DatabaseFileError{
            index::IndexPageError::invalid_page_header}});
    }
    if (header->type == PageType::index_leaf) {
        auto decoded = index::decode_leaf_page(**page);
        if (!decoded) {
            return std::unexpected(BufferPoolError{
                DatabaseFileError{decoded.error()}});
        }
    } else if (header->type == PageType::index_internal) {
        auto decoded = index::decode_internal_page(**page);
        if (!decoded) {
            return std::unexpected(BufferPoolError{
                DatabaseFileError{decoded.error()}});
        }
    } else {
        return std::unexpected(BufferPoolError{DatabaseFileError{
            index::IndexPageError::wrong_page_type}});
    }
    return page;
}

std::expected<WritePageHandle, BufferPoolError>
BufferPool::fetch_index_page_for_write(common::PageId page_id) {
    auto page = fetch_index_page(page_id);
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
        auto written = database_file_.write_page(common::PageId{page_id}, *frame.page);
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

std::expected<void, BufferPoolError> BufferPool::ensure_available_frame() {
    if (pages_.size() < capacity_) {
        return {};
    }
    if (capacity_ == 0) {
        return std::unexpected(BufferPoolError{BufferPoolErrorCode::capacity_exceeded});
    }

    auto evicted = evict_page();
    if (!evicted) {
        return std::unexpected(BufferPoolError{evicted.error()});
    }
    if (!*evicted) {
        return std::unexpected(BufferPoolError{BufferPoolErrorCode::all_pages_pinned});
    }
    return {};
}

std::expected<bool, DatabaseFileError> BufferPool::evict_page() {
    auto candidate = recency_.end();
    while (candidate != recency_.begin()) {
        --candidate;
        auto frame = pages_.find(*candidate);
        if (frame->second.page.use_count() == 1) {
            if (frame->second.dirty) {
                auto written = database_file_.write_page(
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
