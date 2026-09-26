#include "storage/buffer/buffer_pool.hpp"

#include "index/btree_page.hpp"
#include "index/key_codec.hpp"
#include "storage/io/database_file.hpp"
#include "storage/page/slotted_page.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
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
                ("nessodb-buffer-pool-test-" + std::to_string(suffix));
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

void test_cache_heap_pages() {
    TemporaryDirectory directory;
    auto database = nessodb::storage::DatabaseFile::create(
        directory.path() / "database.mdb");
    expect(database.has_value(), "buffer pool database is created");
    if (!database) {
        return;
    }

    const auto first_page_id = database->allocate_heap_page();
    const auto second_page_id = database->allocate_heap_page();
    const auto third_page_id = database->allocate_heap_page();
    expect(first_page_id && second_page_id && third_page_id,
           "buffer pool test pages are allocated");
    if (!first_page_id || !second_page_id || !third_page_id) {
        return;
    }

    nessodb::storage::BufferPool buffer_pool(*database, 2);
    expect(buffer_pool.capacity() == 2, "buffer pool preserves its fixed capacity");
    expect(buffer_pool.size() == 0, "new buffer pool is empty");

    const auto first_fetch = buffer_pool.fetch_heap_page(*first_page_id);
    expect(first_fetch.has_value(), "first heap page is loaded");
    expect(buffer_pool.size() == 1, "first load occupies one cache entry");

    const auto cached_fetch = buffer_pool.fetch_heap_page(*first_page_id);
    expect(cached_fetch.has_value(), "cached heap page is fetched again");
    if (first_fetch && cached_fetch) {
        expect(first_fetch->get() == cached_fetch->get(),
               "repeated fetch returns the cached page instance");
    }
    expect(buffer_pool.size() == 1, "cache hit does not add another entry");

    expect(buffer_pool.fetch_heap_page(*second_page_id).has_value(),
           "second heap page fills the cache");
    const auto third_fetch = buffer_pool.fetch_heap_page(*third_page_id);
    expect(third_fetch.has_value(), "least recently used unpinned page is evicted");
    expect(buffer_pool.size() == 2, "eviction keeps cache at fixed capacity");

    const auto pinned = buffer_pool.fetch_heap_page(*second_page_id);
    expect(!pinned && std::holds_alternative<nessodb::storage::BufferPoolErrorCode>(
                          pinned.error()) &&
               std::get<nessodb::storage::BufferPoolErrorCode>(pinned.error()) ==
                   nessodb::storage::BufferPoolErrorCode::all_pages_pinned,
           "cache refuses eviction while every resident page is pinned");
}

void test_lru_eviction_order() {
    TemporaryDirectory directory;
    auto database = nessodb::storage::DatabaseFile::create(
        directory.path() / "lru.mdb");
    expect(database.has_value(), "LRU test database is created");
    if (!database) {
        return;
    }

    const auto first_page_id = database->allocate_heap_page();
    const auto second_page_id = database->allocate_heap_page();
    const auto third_page_id = database->allocate_heap_page();
    if (!first_page_id || !second_page_id || !third_page_id) {
        expect(false, "LRU test pages are allocated");
        return;
    }

    nessodb::storage::BufferPool buffer_pool(*database, 2);
    std::weak_ptr<const nessodb::storage::PageBuffer> first_page;
    std::weak_ptr<const nessodb::storage::PageBuffer> second_page;
    {
        auto fetched = buffer_pool.fetch_heap_page(*first_page_id);
        expect(fetched.has_value(), "first LRU page is fetched");
        if (fetched) {
            first_page = *fetched;
        }
    }
    {
        auto fetched = buffer_pool.fetch_heap_page(*second_page_id);
        expect(fetched.has_value(), "second LRU page is fetched");
        if (fetched) {
            second_page = *fetched;
        }
    }
    expect(buffer_pool.fetch_heap_page(*first_page_id).has_value(),
           "cache hit refreshes page recency");
    expect(buffer_pool.fetch_heap_page(*third_page_id).has_value(),
           "third page triggers LRU eviction");

    expect(!first_page.expired(), "recently used page remains cached");
    expect(second_page.expired(), "least recently used page is evicted");
}

void test_dirty_page_writeback() {
    TemporaryDirectory directory;
    auto database = nessodb::storage::DatabaseFile::create(
        directory.path() / "writeback.mdb");
    expect(database.has_value(), "write-back test database is created");
    if (!database) {
        return;
    }
    const auto first_page_id = database->allocate_heap_page();
    const auto second_page_id = database->allocate_heap_page();
    if (!first_page_id || !second_page_id) {
        expect(false, "write-back test pages are allocated");
        return;
    }

    constexpr std::array record{std::byte{7}, std::byte{8}, std::byte{9}};
    nessodb::storage::SlotId slot_id;
    nessodb::storage::BufferPool buffer_pool(*database, 1);
    {
        auto writable = buffer_pool.fetch_heap_page_for_write(*first_page_id);
        expect(writable.has_value(), "heap page is fetched for writing");
        if (!writable) {
            return;
        }
        auto slotted_page = nessodb::storage::SlottedPage::open(**writable);
        expect(slotted_page.has_value(), "writable heap page has a valid layout");
        if (!slotted_page) {
            return;
        }
        auto inserted = slotted_page->insert(record);
        expect(inserted.has_value(), "record is inserted into dirty buffer page");
        if (!inserted) {
            return;
        }
        slot_id = *inserted;
    }

    expect(buffer_pool.fetch_heap_page(*second_page_id).has_value(),
           "loading another page evicts and writes the dirty page");
    auto persisted = database->read_heap_page(*first_page_id);
    expect(persisted.has_value(), "evicted dirty page is readable from storage");
    if (persisted) {
        auto slotted_page = nessodb::storage::SlottedPage::open(*persisted);
        expect(slotted_page.has_value(), "evicted dirty page keeps a valid layout");
        if (slotted_page) {
            const auto stored = slotted_page->read(slot_id);
            expect(stored && std::ranges::equal(*stored, record),
                   "dirty page contents are written before eviction");
        }
    }

    nessodb::storage::SlotId second_slot_id;
    {
        auto writable = buffer_pool.fetch_heap_page_for_write(*second_page_id);
        expect(writable.has_value(), "resident page is fetched for explicit flush");
        if (!writable) {
            return;
        }
        expect(buffer_pool.flush().has_value(),
               "explicit flush can write a page with an active handle");

        auto slotted_page = nessodb::storage::SlottedPage::open(**writable);
        if (!slotted_page) {
            expect(false, "active writable page keeps a valid layout");
            return;
        }
        auto inserted = slotted_page->insert(record);
        expect(inserted.has_value(), "page remains writable after an early flush");
        if (!inserted) {
            return;
        }
        second_slot_id = *inserted;
    }
    expect(buffer_pool.flush().has_value(),
           "page modified after an early flush remains dirty");

    auto explicitly_flushed = database->read_heap_page(*second_page_id);
    expect(explicitly_flushed.has_value(), "explicitly flushed page is readable from storage");
    if (explicitly_flushed) {
        auto slotted_page = nessodb::storage::SlottedPage::open(*explicitly_flushed);
        if (slotted_page) {
            const auto stored = slotted_page->read(second_slot_id);
            expect(stored && std::ranges::equal(*stored, record),
                   "changes after an early flush are persisted by the next flush");
        } else {
            expect(false, "explicitly flushed page keeps a valid layout");
        }
    }
}

void test_allocate_buffered_pages() {
    TemporaryDirectory directory;
    auto database = nessodb::storage::DatabaseFile::create(
        directory.path() / "allocation.mdb");
    expect(database.has_value(), "buffered allocation database is created");
    if (!database) {
        return;
    }

    nessodb::storage::BufferPool buffer_pool(*database, 1);
    {
        auto first = buffer_pool.allocate_heap_page();
        expect(first.has_value(), "heap page is allocated through the buffer pool");
        if (!first) {
            return;
        }
        expect(first->page_id == nessodb::common::PageId{1},
               "buffered allocation returns the new page identifier");
        expect(first->page != nullptr, "buffered allocation returns a writable handle");

        const auto blocked = buffer_pool.allocate_heap_page();
        expect(!blocked &&
                   std::holds_alternative<nessodb::storage::BufferPoolErrorCode>(
                       blocked.error()) &&
                   std::get<nessodb::storage::BufferPoolErrorCode>(blocked.error()) ==
                       nessodb::storage::BufferPoolErrorCode::all_pages_pinned,
               "allocation is rejected while the only frame is pinned");
        expect(database->header().page_count == 2,
               "rejected buffered allocation does not change database page count");
    }

    auto second = buffer_pool.allocate_heap_page();
    expect(second.has_value(), "allocation succeeds after the first page is unpinned");
    if (second) {
        expect(second->page_id == nessodb::common::PageId{2},
               "next buffered allocation receives the next page identifier");
    }
    expect(database->header().page_count == 3,
           "successful buffered allocations update database page count");
}

void test_buffer_index_pages() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "index-pages.mdb";
    auto database = nessodb::storage::DatabaseFile::create(path);
    expect(database.has_value(), "index buffer pool database is created");
    if (!database) {
        return;
    }

    nessodb::common::PageId leaf_page_id;
    nessodb::common::PageId internal_page_id;
    const auto key = nessodb::index::encode_key(
        std::vector<nessodb::types::Value>{std::int64_t{42}});
    nessodb::storage::BufferPool buffer_pool(*database, 2);
    {
        auto leaf = buffer_pool.allocate_index_leaf_page();
        expect(leaf.has_value(), "index leaf is allocated through the buffer pool");
        if (!leaf) {
            return;
        }
        leaf_page_id = leaf->page_id;
        expect(nessodb::index::decode_leaf_page(*leaf->page).has_value(),
               "buffered index leaf has a valid empty layout");

        auto internal = buffer_pool.allocate_index_internal_page(1, leaf_page_id);
        expect(internal.has_value(),
               "index internal page is allocated through the buffer pool");
        if (!internal) {
            return;
        }
        internal_page_id = internal->page_id;
        const auto decoded_internal =
            nessodb::index::decode_internal_page(*internal->page);
        expect(decoded_internal &&
                   decoded_internal->leftmost_child == leaf_page_id &&
                   decoded_internal->level == 1,
               "buffered internal page preserves its level and first child");

        const nessodb::index::LeafPage contents{
            leaf_page_id,
            internal_page_id,
            std::nullopt,
            {{key,
              {nessodb::common::PageId{99}, nessodb::storage::SlotId{3}}}},
        };
        expect(nessodb::index::encode_leaf_page(*leaf->page, contents).has_value(),
               "buffered index leaf can be modified through its write handle");

        (*leaf->page)[0] = std::byte{0};
        const auto corrupted_fetch = buffer_pool.fetch_index_page(leaf_page_id);
        expect(!corrupted_fetch,
               "buffer pool rejects a structurally invalid cached index page");
        expect(nessodb::index::encode_leaf_page(*leaf->page, contents).has_value(),
               "valid index contents can replace the corrupted cached page");

        const auto heap_fetch = buffer_pool.fetch_heap_page(leaf_page_id);
        expect(!heap_fetch,
               "buffer pool rejects an index page requested as a heap page");
    }

    expect(buffer_pool.flush().has_value(), "dirty index pages are flushed");
    const auto persisted = database->read_index_page(leaf_page_id);
    expect(persisted.has_value(), "flushed index leaf is readable from storage");
    if (persisted) {
        const auto decoded = nessodb::index::decode_leaf_page(*persisted);
        expect(decoded && decoded->parent_page_id == internal_page_id &&
                   decoded->entries.size() == 1 &&
                   decoded->entries[0].key == key,
               "buffered index changes persist with their tree links and key");
    }

    const auto heap = buffer_pool.allocate_heap_page();
    expect(heap.has_value(), "heap allocation can share the index page cache");
    if (heap) {
        const auto index_fetch = buffer_pool.fetch_index_page(heap->page_id);
        expect(!index_fetch,
               "buffer pool rejects a heap page requested as an index page");
    }
}

}  // namespace

int main() {
    test_cache_heap_pages();
    test_lru_eviction_order();
    test_dirty_page_writeback();
    test_allocate_buffered_pages();
    test_buffer_index_pages();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
