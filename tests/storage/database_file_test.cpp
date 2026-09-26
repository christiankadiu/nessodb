#include "storage/io/database_file.hpp"

#include "common/page_id.hpp"
#include "index/btree_page.hpp"
#include "index/key_codec.hpp"
#include "storage/io/page_file.hpp"
#include "storage/page/checksum.hpp"
#include "storage/page/database_header.hpp"
#include "storage/page/page.hpp"
#include "storage/page/slotted_page.hpp"

#include <algorithm>
#include <array>
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
                ("nessodb-database-file-test-" + std::to_string(suffix));
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

void test_create_and_reopen_database() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "database.mdb";

    {
        auto created = nessodb::storage::DatabaseFile::create(path);
        expect(created.has_value(), "database file is created");
        if (created) {
            expect(created->header() == nessodb::storage::DatabaseHeader{},
                   "new database exposes its initial header");
            expect(created->path() == path, "database preserves its path");
        }
    }

    {
        auto raw_file = nessodb::storage::PageFile::open(path);
        expect(raw_file.has_value(), "created database can be inspected as a page file");
        if (raw_file) {
            auto header_page = raw_file->read_page(nessodb::common::PageId{0});
            expect(header_page.has_value(), "created database contains page zero");
            if (header_page) {
                expect(nessodb::storage::verify_page_checksum(*header_page).has_value(),
                       "database header page has a valid checksum");
                expect(nessodb::storage::decode_database_header(*header_page).has_value(),
                       "database header page has a valid format");
            }
        }
    }

    auto reopened = nessodb::storage::DatabaseFile::open(path);
    expect(reopened.has_value(), "valid database file is reopened");
}

void test_reject_page_count_mismatch() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "mismatch.mdb";
    {
        auto database = nessodb::storage::DatabaseFile::create(path);
        expect(database.has_value(), "mismatch test database is created");
    }

    {
        auto raw_file = nessodb::storage::PageFile::open(path);
        expect(raw_file.has_value(), "mismatch test page file is opened");
        if (!raw_file) {
            return;
        }
        nessodb::storage::PageBuffer extra_page{};
        expect(raw_file->write_page(nessodb::common::PageId{1}, extra_page).has_value(),
               "extra physical page is appended");
        expect(raw_file->flush().has_value(), "extra physical page is flushed");
    }

    auto reopened = nessodb::storage::DatabaseFile::open(path);
    expect(!reopened &&
               std::holds_alternative<nessodb::storage::DatabaseFileStructureError>(
                   reopened.error()),
           "physical and logical page count mismatch is rejected");
}

void test_reject_corrupted_header() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "corrupted.mdb";
    {
        auto database = nessodb::storage::DatabaseFile::create(path);
        expect(database.has_value(), "corruption test database is created");
    }

    {
        auto raw_file = nessodb::storage::PageFile::open(path);
        expect(raw_file.has_value(), "corruption test page file is opened");
        if (!raw_file) {
            return;
        }
        auto header_page = raw_file->read_page(nessodb::common::PageId{0});
        expect(header_page.has_value(), "corruption test header is read");
        if (!header_page) {
            return;
        }
        header_page->back() = std::byte{1};
        expect(raw_file->write_page(nessodb::common::PageId{0}, *header_page).has_value(),
               "corrupted header is written");
        expect(raw_file->flush().has_value(), "corrupted header is flushed");
    }

    auto reopened = nessodb::storage::DatabaseFile::open(path);
    expect(!reopened && std::holds_alternative<nessodb::storage::PageChecksumError>(
                            reopened.error()),
           "database with corrupted header checksum is rejected");
}

void test_allocate_heap_pages() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "allocation.mdb";
    {
        auto database = nessodb::storage::DatabaseFile::create(path);
        expect(database.has_value(), "allocation test database is created");
        if (!database) {
            return;
        }

        const auto first_page = database->allocate_heap_page();
        const auto second_page = database->allocate_heap_page();
        expect(first_page == nessodb::common::PageId{1},
               "first heap page receives page identifier one");
        expect(second_page == nessodb::common::PageId{2},
               "second heap page receives the next identifier");
        expect(database->header().page_count == 3,
               "allocation updates the in-memory page count");
    }

    auto reopened = nessodb::storage::DatabaseFile::open(path);
    expect(reopened.has_value(), "database with allocated pages is reopened");
    if (reopened) {
        expect(reopened->header().page_count == 3,
               "updated page count persists across reopening");
    }

    auto raw_file = nessodb::storage::PageFile::open(path);
    expect(raw_file.has_value(), "allocated pages can be inspected");
    if (!raw_file) {
        return;
    }
    for (std::uint64_t value = 1; value <= 2; ++value) {
        auto page = raw_file->read_page(nessodb::common::PageId{value});
        expect(page.has_value(), "allocated heap page is readable");
        if (page) {
            expect(nessodb::storage::verify_page_checksum(*page).has_value(),
                   "allocated heap page has a valid checksum");
            expect(nessodb::storage::SlottedPage::open(*page).has_value(),
                   "allocated heap page has a valid empty layout");
        }
    }
}

void test_initialize_catalog_root() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "catalog-root.mdb";
    {
        auto database = nessodb::storage::DatabaseFile::create(path);
        if (!database) {
            expect(false, "catalog root test database is created");
            return;
        }

        const auto invalid = database->initialize_catalog_root(
            nessodb::common::PageId{1});
        expect(!invalid &&
                   std::holds_alternative<nessodb::storage::DatabaseHeaderError>(
                       invalid.error()),
               "catalog root must reference an allocated page");

        const auto allocated = database->allocate_heap_page();
        expect(allocated.has_value(), "catalog root page is allocated");
        if (!allocated) {
            return;
        }
        expect(database->initialize_catalog_root(*allocated).has_value(),
               "catalog root is initialized");
        expect(database->header().catalog_root == *allocated,
               "catalog root updates the in-memory header");

        const auto replacement = database->initialize_catalog_root(*allocated);
        expect(!replacement &&
                   std::holds_alternative<
                       nessodb::storage::DatabaseFileStructureError>(
                       replacement.error()) &&
                   std::get<nessodb::storage::DatabaseFileStructureError>(
                       replacement.error()) ==
                       nessodb::storage::DatabaseFileStructureError::
                           catalog_root_already_initialized,
               "catalog root cannot be initialized twice");
    }

    const auto reopened = nessodb::storage::DatabaseFile::open(path);
    expect(reopened &&
               reopened->header().catalog_root == nessodb::common::PageId{1},
           "catalog root persists across reopening");
}

void test_persist_heap_page() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "heap-page.mdb";
    nessodb::common::PageId page_id;
    nessodb::storage::SlotId slot_id;
    constexpr std::array record{std::byte{1}, std::byte{2}, std::byte{3}};

    {
        auto database = nessodb::storage::DatabaseFile::create(path);
        expect(database.has_value(), "heap persistence database is created");
        if (!database) {
            return;
        }
        auto allocated = database->allocate_heap_page();
        expect(allocated.has_value(), "heap persistence page is allocated");
        if (!allocated) {
            return;
        }
        page_id = *allocated;

        auto page = database->read_heap_page(page_id);
        expect(page.has_value(), "allocated heap page is read through database file");
        if (!page) {
            return;
        }
        auto slotted_page = nessodb::storage::SlottedPage::open(*page);
        expect(slotted_page.has_value(), "read heap page can be edited");
        if (!slotted_page) {
            return;
        }
        auto inserted = slotted_page->insert(record);
        expect(inserted.has_value(), "record is inserted into heap page buffer");
        if (!inserted) {
            return;
        }
        slot_id = *inserted;
        expect(database->write_heap_page(page_id, *page).has_value(),
               "modified heap page is persisted");
    }

    auto reopened = nessodb::storage::DatabaseFile::open(path);
    expect(reopened.has_value(), "heap persistence database is reopened");
    if (!reopened) {
        return;
    }
    auto page = reopened->read_heap_page(page_id);
    expect(page.has_value(), "persisted heap page is read after reopening");
    if (!page) {
        return;
    }
    auto slotted_page = nessodb::storage::SlottedPage::open(*page);
    expect(slotted_page.has_value(), "persisted heap page remains valid");
    if (slotted_page) {
        const auto stored_record = slotted_page->read(slot_id);
        expect(stored_record && std::ranges::equal(*stored_record, record),
               "persisted record bytes round-trip");
    }
}

void test_persist_index_pages() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "index-pages.mdb";
    nessodb::common::PageId leaf_page_id;
    nessodb::common::PageId internal_page_id;
    const std::vector<nessodb::types::Value> values{std::int64_t{42}};
    const auto key = nessodb::index::encode_key(values);

    {
        auto database = nessodb::storage::DatabaseFile::create(path);
        expect(database.has_value(), "index persistence database is created");
        if (!database) {
            return;
        }
        const auto leaf = database->allocate_index_leaf_page();
        expect(leaf.has_value(), "index leaf page is allocated");
        if (!leaf) {
            return;
        }
        leaf_page_id = *leaf;
        const auto internal = database->allocate_index_internal_page(1, *leaf);
        expect(internal.has_value(), "index internal page is allocated");
        if (!internal) {
            return;
        }
        internal_page_id = *internal;

        auto page = database->read_index_page(*leaf);
        expect(page.has_value(), "allocated index leaf is read");
        if (!page) {
            return;
        }
        nessodb::index::LeafPage contents{
            *leaf, *internal, std::nullopt,
            {{key, {nessodb::common::PageId{99}, nessodb::storage::SlotId{3}}}}};
        expect(nessodb::index::encode_leaf_page(*page, contents).has_value() &&
                   database->write_index_page(*leaf, *page).has_value(),
               "modified index leaf is persisted");
        expect(!database->read_heap_page(*leaf),
               "index page is rejected by heap-only reads");
    }

    auto reopened = nessodb::storage::DatabaseFile::open(path);
    expect(reopened && reopened->header().page_count == 3,
           "database with index pages reopens with its page count");
    if (!reopened) {
        return;
    }
    const auto leaf_page = reopened->read_index_page(leaf_page_id);
    const auto internal_page = reopened->read_index_page(internal_page_id);
    expect(leaf_page && internal_page,
           "both index page kinds survive reopening");
    if (leaf_page) {
        const auto decoded = nessodb::index::decode_leaf_page(*leaf_page);
        expect(decoded && decoded->entries.size() == 1 &&
                   decoded->entries[0].key == key &&
                   decoded->parent_page_id == internal_page_id,
               "persisted index leaf contents round-trip");
    }
}

}  // namespace

int main() {
    test_create_and_reopen_database();
    test_reject_page_count_mismatch();
    test_reject_corrupted_header();
    test_allocate_heap_pages();
    test_initialize_catalog_root();
    test_persist_heap_page();
    test_persist_index_pages();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
