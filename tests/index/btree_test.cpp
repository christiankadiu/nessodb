#include "index/btree.hpp"

#include "index/key_codec.hpp"
#include "storage/buffer/buffer_pool.hpp"
#include "storage/io/database_file.hpp"
#include "types/value.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using nessodb::common::PageId;
using nessodb::index::BTree;
using nessodb::index::BTreeCursor;
using nessodb::index::BTreeErrorCode;
using nessodb::index::BTreeKeyBound;
using nessodb::index::BTreeRange;
using nessodb::index::EncodedKey;
using nessodb::index::LeafEntry;
using nessodb::storage::RecordId;
using nessodb::storage::SlotId;
using nessodb::types::Value;

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
                ("nessodb-btree-test-" + std::to_string(suffix));
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

EncodedKey integer_key(std::int64_t value) {
    return nessodb::index::encode_key(std::vector<Value>{value});
}

EncodedKey text_key(std::string value) {
    return nessodb::index::encode_key(
        std::vector<Value>{std::move(value)});
}

std::optional<std::vector<LeafEntry>> collect(BTreeCursor cursor) {
    std::vector<LeafEntry> entries;
    while (true) {
        auto entry = cursor.next();
        if (!entry) {
            expect(false, "B+ tree cursor advances without errors");
            return std::nullopt;
        }
        if (!*entry) {
            return entries;
        }
        entries.push_back(std::move(**entry));
    }
}

void test_single_leaf_operations() {
    TemporaryDirectory directory;
    auto database = nessodb::storage::DatabaseFile::create(
        directory.path() / "operations.mdb");
    expect(database.has_value(), "B+ tree database is created");
    if (!database) {
        return;
    }
    nessodb::storage::BufferPool buffer_pool(*database, 4);
    auto tree = BTree::create(buffer_pool);
    expect(tree.has_value(), "B+ tree is created with a leaf root");
    if (!tree) {
        return;
    }
    expect(tree->root_page_id() == PageId{1},
           "new B+ tree exposes its root page identifier");

    const auto first_key = integer_key(1);
    const auto second_key = integer_key(2);
    const RecordId first_record{PageId{20}, SlotId{4}};
    const RecordId second_record{PageId{10}, SlotId{3}};
    const RecordId third_record{PageId{20}, SlotId{1}};

    expect(tree->insert(second_key, first_record).has_value() &&
               tree->insert(first_key, second_record).has_value() &&
               tree->insert(second_key, third_record).has_value(),
           "entries can be inserted out of key and record order");

    const auto matches = tree->find(second_key);
    expect(matches &&
               *matches == std::vector<RecordId>{third_record, first_record},
           "exact lookup returns duplicate keys in record identifier order");
    const auto missing = tree->find(integer_key(3));
    expect(missing && missing->empty(),
           "exact lookup returns an empty result for a missing key");

    const auto duplicate = tree->insert(second_key, first_record);
    expect(!duplicate &&
               std::holds_alternative<BTreeErrorCode>(duplicate.error()) &&
               std::get<BTreeErrorCode>(duplicate.error()) ==
                   BTreeErrorCode::duplicate_entry,
           "the same key and record pair cannot be inserted twice");

    const auto invalid_record = tree->insert(
        first_key, RecordId{PageId{0}, SlotId{0}});
    expect(!invalid_record &&
               std::holds_alternative<BTreeErrorCode>(invalid_record.error()) &&
               std::get<BTreeErrorCode>(invalid_record.error()) ==
                   BTreeErrorCode::invalid_record_id,
           "invalid record identifiers are rejected");

    const EncodedKey malformed_key{std::byte{99}};
    const auto invalid_key = tree->find(malformed_key);
    expect(!invalid_key &&
               std::holds_alternative<nessodb::index::IndexPageError>(
                   invalid_key.error()) &&
               std::get<nessodb::index::IndexPageError>(invalid_key.error()) ==
                   nessodb::index::IndexPageError::invalid_key,
           "malformed lookup keys are rejected");

    const auto oversized = tree->insert(
        text_key(std::string(5000, 'x')),
        RecordId{PageId{30}, SlotId{0}});
    expect(!oversized &&
               std::holds_alternative<nessodb::index::IndexPageError>(
                   oversized.error()) &&
               std::get<nessodb::index::IndexPageError>(oversized.error()) ==
                   nessodb::index::IndexPageError::page_full,
           "an insertion that does not fit reports a full leaf page");
    expect(tree->find(second_key) == matches,
           "failed insertions leave existing leaf contents unchanged");

    expect(tree->erase(second_key, third_record).has_value(),
           "an exact key and record pair can be erased");
    expect(tree->find(second_key) == std::vector<RecordId>{first_record},
           "erasing one duplicate leaves the other record intact");
    const auto missing_entry = tree->erase(second_key, third_record);
    expect(!missing_entry &&
               std::holds_alternative<BTreeErrorCode>(
                   missing_entry.error()) &&
               std::get<BTreeErrorCode>(missing_entry.error()) ==
                   BTreeErrorCode::entry_not_found,
           "erasing a missing key and record pair reports an error");

    const auto invalid_erase = tree->erase(
        first_key, RecordId{PageId{0}, SlotId{0}});
    expect(!invalid_erase &&
               std::holds_alternative<BTreeErrorCode>(
                   invalid_erase.error()) &&
               std::get<BTreeErrorCode>(invalid_erase.error()) ==
                   BTreeErrorCode::invalid_record_id,
           "erasing an invalid record identifier is rejected");
}

void test_reopen_tree() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "persistence.mdb";
    const auto key = integer_key(42);
    const RecordId record_id{PageId{7}, SlotId{2}};
    PageId root_page_id;

    {
        auto database = nessodb::storage::DatabaseFile::create(path);
        if (!database) {
            expect(false, "persistent B+ tree database is created");
            return;
        }
        nessodb::storage::BufferPool buffer_pool(*database, 2);
        auto tree = BTree::create(buffer_pool);
        if (!tree) {
            expect(false, "persistent B+ tree is created");
            return;
        }
        root_page_id = tree->root_page_id();
        expect(tree->insert(key, record_id).has_value(),
               "persistent B+ tree receives an entry");
        expect(buffer_pool.flush().has_value(),
               "persistent B+ tree pages are flushed");
    }

    auto reopened_database = nessodb::storage::DatabaseFile::open(path);
    expect(reopened_database.has_value(), "B+ tree database is reopened");
    if (!reopened_database) {
        return;
    }
    nessodb::storage::BufferPool buffer_pool(*reopened_database, 2);
    auto reopened_tree = BTree::open(buffer_pool, root_page_id);
    expect(reopened_tree.has_value(), "B+ tree is reopened from its root page");
    if (reopened_tree) {
        const auto matches = reopened_tree->find(key);
        expect(matches && *matches == std::vector<RecordId>{record_id},
               "reopened B+ tree returns its persisted entry");
    }

    const auto invalid_root = BTree::open(buffer_pool, PageId{0});
    expect(!invalid_root &&
               std::holds_alternative<BTreeErrorCode>(invalid_root.error()) &&
               std::get<BTreeErrorCode>(invalid_root.error()) ==
                   BTreeErrorCode::invalid_root_page_id,
           "page zero cannot be used as a B+ tree root");
}

void test_range_scan() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "range-scan.mdb";
    std::vector<LeafEntry> entries;
    for (std::uint64_t value = 0; value < 12; ++value) {
        entries.push_back(LeafEntry{
            text_key(std::string(700, static_cast<char>('a' + value))),
            RecordId{PageId{100 + value}, SlotId{0}},
        });
    }
    BTreeRange range{
        BTreeKeyBound{entries[3].key, false},
        BTreeKeyBound{entries[9].key, true},
    };
    const std::vector<LeafEntry> expected(entries.begin() + 4,
                                          entries.begin() + 10);
    PageId root_page_id;

    {
        auto database = nessodb::storage::DatabaseFile::create(path);
        expect(database.has_value(), "range scan database is created");
        if (!database) {
            return;
        }
        nessodb::storage::BufferPool buffer_pool(*database, 1);
        auto tree = BTree::create(buffer_pool);
        expect(tree.has_value(), "range scan tree is created");
        if (!tree) {
            return;
        }
        for (const auto& entry : entries) {
            const auto inserted = tree->insert(entry.key, entry.record_id);
            expect(inserted.has_value(),
                   "range scan entries are inserted");
            if (!inserted) {
                return;
            }
        }
        root_page_id = tree->root_page_id();

        auto cursor = tree->scan(range);
        expect(cursor.has_value(), "bounded range cursor is created");
        if (cursor) {
            expect(collect(std::move(*cursor)) == expected,
                   "range cursor honors inclusive and exclusive bounds");
        }

        BTreeRange single_key{
            BTreeKeyBound{entries[5].key, true},
            BTreeKeyBound{entries[5].key, true},
        };
        auto single_cursor = tree->scan(std::move(single_key));
        expect(single_cursor.has_value(), "single-key cursor is created");
        if (single_cursor) {
            expect(collect(std::move(*single_cursor)) ==
                       std::vector<LeafEntry>{entries[5]},
                   "equal inclusive bounds return their key");
        }

        BTreeRange empty_range{
            BTreeKeyBound{entries[5].key, false},
            BTreeKeyBound{entries[5].key, true},
        };
        auto empty_cursor = tree->scan(std::move(empty_range));
        expect(empty_cursor.has_value(), "empty cursor is created");
        if (empty_cursor) {
            expect(collect(std::move(*empty_cursor)) ==
                       std::vector<LeafEntry>{},
                   "an exclusive equal lower bound produces an empty range");
        }

        auto all_cursor = tree->scan();
        expect(all_cursor.has_value(), "unbounded cursor is created");
        if (all_cursor) {
            expect(collect(std::move(*all_cursor)) == entries,
                   "unbounded cursor crosses every leaf in key order");
        }

        BTreeRange reversed{
            BTreeKeyBound{entries[8].key, true},
            BTreeKeyBound{entries[2].key, true},
        };
        const auto invalid_range = tree->scan(std::move(reversed));
        expect(!invalid_range &&
                   std::holds_alternative<BTreeErrorCode>(
                       invalid_range.error()) &&
                   std::get<BTreeErrorCode>(invalid_range.error()) ==
                       BTreeErrorCode::invalid_range,
               "reversed range bounds are rejected");

        BTreeRange malformed{
            BTreeKeyBound{EncodedKey{std::byte{99}}, true},
            std::nullopt,
        };
        const auto invalid_key = tree->scan(std::move(malformed));
        expect(!invalid_key &&
                   std::holds_alternative<nessodb::index::IndexPageError>(
                       invalid_key.error()) &&
                   std::get<nessodb::index::IndexPageError>(
                       invalid_key.error()) ==
                       nessodb::index::IndexPageError::invalid_key,
               "malformed range bounds are rejected");
        expect(buffer_pool.flush().has_value(),
               "range scan tree is flushed");
    }

    auto database = nessodb::storage::DatabaseFile::open(path);
    expect(database.has_value(), "range scan database is reopened");
    if (!database) {
        return;
    }
    nessodb::storage::BufferPool buffer_pool(*database, 1);
    auto tree = BTree::open(buffer_pool, root_page_id);
    expect(tree.has_value(), "range scan tree is reopened");
    if (!tree) {
        return;
    }
    auto cursor = tree->scan(range);
    expect(cursor.has_value(), "reopened range cursor is created");
    if (cursor) {
        expect(collect(std::move(*cursor)) == expected,
               "range scan survives reopening");
    }
}

void test_erase_across_leaf_chain() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "erase-leaf-chain.mdb";
    const auto key = text_key(std::string(700, 'm'));
    std::vector<RecordId> records;
    for (std::uint64_t value = 0; value < 12; ++value) {
        records.push_back(RecordId{PageId{100 + value}, SlotId{0}});
    }
    const RecordId erased_record = records[10];
    PageId root_page_id;

    {
        auto database = nessodb::storage::DatabaseFile::create(path);
        expect(database.has_value(), "erase database is created");
        if (!database) {
            return;
        }
        nessodb::storage::BufferPool buffer_pool(*database, 1);
        auto tree = BTree::create(buffer_pool);
        expect(tree.has_value(), "erase tree is created");
        if (!tree) {
            return;
        }
        for (const auto record_id : records) {
            const auto inserted = tree->insert(key, record_id);
            expect(inserted.has_value(),
                   "duplicate erase entries are inserted");
            if (!inserted) {
                return;
            }
        }
        root_page_id = tree->root_page_id();

        expect(tree->erase(key, erased_record).has_value(),
               "an entry is erased after crossing duplicate leaf pages");
        records.erase(records.begin() + 10);
        expect(tree->find(key) == records,
               "cross-leaf erase preserves every other duplicate");

        const auto missing = tree->erase(key, erased_record);
        expect(!missing &&
                   std::holds_alternative<BTreeErrorCode>(missing.error()) &&
                   std::get<BTreeErrorCode>(missing.error()) ==
                       BTreeErrorCode::entry_not_found,
               "a removed cross-leaf entry cannot be erased twice");
        expect(buffer_pool.flush().has_value(),
               "erased leaf pages are flushed");
    }

    auto database = nessodb::storage::DatabaseFile::open(path);
    expect(database.has_value(), "erase database is reopened");
    if (!database) {
        return;
    }
    nessodb::storage::BufferPool buffer_pool(*database, 1);
    auto tree = BTree::open(buffer_pool, root_page_id);
    expect(tree.has_value(), "erase tree is reopened");
    if (tree) {
        expect(tree->find(key) == records,
               "cross-leaf deletion survives reopening");
    }
}

void test_leaf_redistribution_after_erase() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "leaf-redistribution.mdb";
    std::vector<LeafEntry> entries;
    for (std::uint64_t value = 0; value < 7; ++value) {
        entries.push_back(LeafEntry{
            text_key(std::string(700, static_cast<char>('a' + value))),
            RecordId{PageId{200 + value}, SlotId{0}},
        });
    }
    PageId root_page_id;

    {
        auto database = nessodb::storage::DatabaseFile::create(path);
        expect(database.has_value(), "redistribution database is created");
        if (!database) {
            return;
        }
        nessodb::storage::BufferPool buffer_pool(*database, 1);
        auto tree = BTree::create(buffer_pool);
        expect(tree.has_value(), "redistribution tree is created");
        if (!tree) {
            return;
        }
        for (const auto& entry : entries) {
            if (!tree->insert(entry.key, entry.record_id)) {
                expect(false, "redistribution entries are inserted");
                return;
            }
        }
        root_page_id = tree->root_page_id();

        expect(tree->erase(entries[0].key, entries[0].record_id).has_value(),
               "underfilled leaf entry is erased");
        expect(tree->root_page_id() == root_page_id,
               "redistribution keeps the internal root");
        const std::vector<LeafEntry> expected(entries.begin() + 1,
                                              entries.end());
        auto cursor = tree->scan();
        expect(cursor && collect(std::move(*cursor)) == expected,
               "redistribution preserves ordered scan contents");

        auto root = buffer_pool.fetch_index_page(root_page_id);
        expect(root.has_value(), "redistributed root page is fetched");
        if (root) {
            auto decoded = nessodb::index::decode_internal_page(**root);
            expect(decoded && decoded->entries.size() == 1 &&
                       decoded->entries.front().key == entries[4].key,
                   "redistribution updates the parent separator");
        }
        expect(buffer_pool.flush().has_value(),
               "redistributed tree is flushed");
    }

    auto database = nessodb::storage::DatabaseFile::open(path);
    expect(database.has_value(), "redistribution database is reopened");
    if (!database) {
        return;
    }
    nessodb::storage::BufferPool buffer_pool(*database, 1);
    auto tree = BTree::open(buffer_pool, root_page_id);
    expect(tree.has_value(), "redistributed tree is reopened");
    const std::vector<LeafEntry> expected(entries.begin() + 1,
                                          entries.end());
    if (tree) {
        auto cursor = tree->scan();
        expect(cursor && collect(std::move(*cursor)) == expected,
               "redistributed leaves survive reopening");
    }
}

void test_leaf_merge_after_erase() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "leaf-merge.mdb";
    std::vector<LeafEntry> entries;
    for (std::uint64_t value = 0; value < 6; ++value) {
        entries.push_back(LeafEntry{
            text_key(std::string(700, static_cast<char>('a' + value))),
            RecordId{PageId{300 + value}, SlotId{0}},
        });
    }
    PageId root_page_id;

    {
        auto database = nessodb::storage::DatabaseFile::create(path);
        expect(database.has_value(), "leaf merge database is created");
        if (!database) {
            return;
        }
        nessodb::storage::BufferPool buffer_pool(*database, 1);
        auto tree = BTree::create(buffer_pool);
        expect(tree.has_value(), "leaf merge tree is created");
        if (!tree) {
            return;
        }
        for (const auto& entry : entries) {
            if (!tree->insert(entry.key, entry.record_id)) {
                expect(false, "leaf merge entries are inserted");
                return;
            }
        }
        const PageId internal_root_page_id = tree->root_page_id();

        expect(tree->erase(entries[0].key, entries[0].record_id).has_value(),
               "merge-triggering leaf entry is erased");
        root_page_id = tree->root_page_id();
        expect(root_page_id != internal_root_page_id,
               "merging the final two leaves collapses the root");

        auto root = buffer_pool.fetch_index_page(root_page_id);
        expect(root.has_value(), "collapsed leaf root is fetched");
        if (root) {
            auto decoded = nessodb::index::decode_leaf_page(**root);
            expect(decoded && !decoded->parent_page_id &&
                       !decoded->next_page_id &&
                       decoded->entries ==
                           std::vector<LeafEntry>(entries.begin() + 1,
                                                  entries.end()),
                   "collapsed root owns all remaining ordered entries");
        }
        expect(buffer_pool.flush().has_value(),
               "merged tree is flushed");
    }

    auto database = nessodb::storage::DatabaseFile::open(path);
    expect(database.has_value(), "leaf merge database is reopened");
    if (!database) {
        return;
    }
    nessodb::storage::BufferPool buffer_pool(*database, 1);
    auto tree = BTree::open(buffer_pool, root_page_id);
    expect(tree.has_value(), "collapsed tree is reopened from its new root");
    if (tree) {
        auto cursor = tree->scan();
        expect(cursor && collect(std::move(*cursor)) ==
                             std::vector<LeafEntry>(entries.begin() + 1,
                                                    entries.end()),
               "merged leaf contents survive reopening");
    }
}

void test_split_leaf_root() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "root-split.mdb";
    const auto duplicate_key = text_key(std::string(700, 'm'));
    const auto left_duplicate_key = text_key(std::string(700, 'a'));
    const auto lower_key = integer_key(1);
    const auto higher_key = text_key("z");
    std::vector<RecordId> duplicate_records;
    std::vector<RecordId> left_duplicate_records;
    PageId root_page_id;

    {
        auto database = nessodb::storage::DatabaseFile::create(path);
        if (!database) {
            expect(false, "root split database is created");
            return;
        }
        nessodb::storage::BufferPool buffer_pool(*database, 1);
        auto tree = BTree::create(buffer_pool);
        if (!tree) {
            expect(false, "root split tree is created");
            return;
        }
        const PageId initial_root_page_id = tree->root_page_id();

        for (std::uint64_t value = 10; value <= 15; ++value) {
            const RecordId record_id{PageId{value}, SlotId{0}};
            duplicate_records.push_back(record_id);
            expect(tree->insert(duplicate_key, record_id).has_value(),
                   "duplicate entry is inserted while filling the root leaf");
        }
        root_page_id = tree->root_page_id();
        expect(root_page_id != initial_root_page_id,
               "a full leaf root is replaced by a new internal root");

        const RecordId late_duplicate{PageId{5}, SlotId{1}};
        duplicate_records.push_back(late_duplicate);
        expect(tree->insert(duplicate_key, late_duplicate).has_value(),
               "duplicate key insertion descends through the internal root");
        expect(tree->insert(lower_key, RecordId{PageId{2}, SlotId{0}})
                       .has_value() &&
                   tree->insert(higher_key, RecordId{PageId{30}, SlotId{0}})
                       .has_value(),
               "keys on both sides of the separator reach the correct leaf");

        const RecordId final_fitting_duplicate{PageId{40}, SlotId{0}};
        duplicate_records.push_back(final_fitting_duplicate);
        expect(tree->insert(duplicate_key, final_fitting_duplicate).has_value(),
               "a non-root leaf accepts another entry while space remains");
        const auto page_count_before_child_split = database->header().page_count;
        const RecordId split_child_record{PageId{41}, SlotId{0}};
        duplicate_records.push_back(split_child_record);
        const auto split_child = tree->insert(duplicate_key, split_child_record);
        expect(split_child.has_value() &&
                   tree->root_page_id() == root_page_id &&
                   database->header().page_count ==
                       page_count_before_child_split + 1,
               "a full non-root leaf splits without replacing the internal root");

        std::sort(duplicate_records.begin(), duplicate_records.end(),
                  [](RecordId left, RecordId right) {
                      if (left.page_id.value != right.page_id.value) {
                          return left.page_id.value < right.page_id.value;
                      }
                      return left.slot_id.value < right.slot_id.value;
                  });
        const auto matches = tree->find(duplicate_key);
        expect(matches && *matches == duplicate_records,
               "lookup follows the leaf chain and returns split duplicates");
        const auto split_duplicate =
            tree->insert(duplicate_key, duplicate_records.front());
        expect(!split_duplicate &&
                   std::holds_alternative<BTreeErrorCode>(
                       split_duplicate.error()) &&
                   std::get<BTreeErrorCode>(split_duplicate.error()) ==
                       BTreeErrorCode::duplicate_entry,
               "duplicate detection spans both leaves after a split");
        expect(tree->find(lower_key) ==
                   std::vector<RecordId>{{PageId{2}, SlotId{0}}} &&
                   tree->find(higher_key) ==
                       std::vector<RecordId>{{PageId{30}, SlotId{0}}},
               "lookup descends to either child of the internal root");

        nessodb::index::InternalPage root_contents;
        {
            auto root_page = buffer_pool.fetch_index_page(root_page_id);
            expect(root_page.has_value(), "new internal root is buffered");
            if (!root_page) {
                return;
            }
            auto decoded = nessodb::index::decode_internal_page(**root_page);
            expect(decoded.has_value(), "new root has an internal page layout");
            if (!decoded) {
                return;
            }
            root_contents = std::move(*decoded);
        }
        expect(root_contents.level == 1 &&
                   root_contents.leftmost_child == initial_root_page_id &&
                   root_contents.entries.size() == 2 &&
                   root_contents.entries[0].key == duplicate_key &&
                   root_contents.entries[1].key == duplicate_key,
               "internal root receives a duplicate separator for the third leaf");

        const PageId middle_page_id =
            root_contents.entries.front().right_child;
        const PageId right_page_id = root_contents.entries.back().right_child;
        {
            auto left_page =
                buffer_pool.fetch_index_page(root_contents.leftmost_child);
            if (!left_page) {
                expect(false, "left split leaf is buffered");
                return;
            }
            const auto left = nessodb::index::decode_leaf_page(**left_page);
            expect(left && left->parent_page_id == root_page_id &&
                       left->next_page_id == middle_page_id,
                   "left leaf points to its parent and middle sibling");
        }
        {
            auto middle_page = buffer_pool.fetch_index_page(middle_page_id);
            if (!middle_page) {
                expect(false, "middle split leaf is buffered");
                return;
            }
            const auto middle =
                nessodb::index::decode_leaf_page(**middle_page);
            expect(middle && middle->parent_page_id == root_page_id &&
                       middle->next_page_id == right_page_id,
                   "middle leaf links the two sides of the leaf chain");
        }
        {
            auto right_page = buffer_pool.fetch_index_page(right_page_id);
            if (!right_page) {
                expect(false, "right split leaf is buffered");
                return;
            }
            const auto right = nessodb::index::decode_leaf_page(**right_page);
            expect(right && right->parent_page_id == root_page_id &&
                       !right->next_page_id,
                   "right split leaf points to its parent and terminates the chain");
        }

        const auto page_count_before_left_split = database->header().page_count;
        for (std::uint64_t value = 50; value <= 52; ++value) {
            const RecordId record_id{PageId{value}, SlotId{0}};
            left_duplicate_records.push_back(record_id);
            expect(tree->insert(left_duplicate_key, record_id).has_value(),
                   "entries fill and split the leftmost child");
        }
        expect(tree->root_page_id() == root_page_id &&
                   database->header().page_count ==
                       page_count_before_left_split + 1 &&
                   tree->find(left_duplicate_key) == left_duplicate_records,
               "leftmost child split preserves the root and all new entries");
        {
            auto root_page = buffer_pool.fetch_index_page(root_page_id);
            if (!root_page) {
                expect(false, "root is buffered after the leftmost split");
                return;
            }
            const auto root =
                nessodb::index::decode_internal_page(**root_page);
            expect(root && root->entries.size() == 3,
                   "leftmost split inserts a separator at the front of the root");
        }
        expect(buffer_pool.flush().has_value(),
               "split root and leaf pages are flushed");
    }

    auto database = nessodb::storage::DatabaseFile::open(path);
    expect(database.has_value(), "database with a split tree is reopened");
    if (!database) {
        return;
    }
    nessodb::storage::BufferPool buffer_pool(*database, 1);
    auto tree = BTree::open(buffer_pool, root_page_id);
    expect(tree.has_value(), "tree with an internal root is reopened");
    if (tree) {
        expect(tree->find(duplicate_key) == duplicate_records &&
                   tree->find(left_duplicate_key) == left_duplicate_records,
               "entries from repeated child splits survive reopening");
    }
}

void test_multilevel_navigation() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "multilevel.mdb";
    const auto low_key = integer_key(1);
    const auto separator_key = integer_key(10);
    const auto high_key = integer_key(20);
    const RecordId left_duplicate{PageId{10}, SlotId{0}};
    const RecordId right_duplicate{PageId{20}, SlotId{0}};
    const RecordId inserted_duplicate{PageId{5}, SlotId{0}};
    PageId root_page_id;

    {
        auto database = nessodb::storage::DatabaseFile::create(path);
        if (!database) {
            expect(false, "multilevel database is created");
            return;
        }
        nessodb::storage::BufferPool buffer_pool(*database, 1);
        const auto allocate_leaf = [&buffer_pool]() {
            auto page = buffer_pool.allocate_index_leaf_page();
            return page ? page->page_id : PageId{};
        };
        const PageId first_leaf_id = allocate_leaf();
        const PageId second_leaf_id = allocate_leaf();
        const PageId third_leaf_id = allocate_leaf();
        auto left_internal = buffer_pool.allocate_index_internal_page(
            1, first_leaf_id);
        if (!first_leaf_id.is_valid() || !second_leaf_id.is_valid() ||
            !third_leaf_id.is_valid() || !left_internal) {
            expect(false, "multilevel leaf and left internal pages are allocated");
            return;
        }
        const PageId left_internal_id = left_internal->page_id;
        left_internal->page.reset();
        auto right_internal = buffer_pool.allocate_index_internal_page(
            1, third_leaf_id);
        if (!right_internal) {
            expect(false, "multilevel right internal page is allocated");
            return;
        }
        const PageId right_internal_id = right_internal->page_id;
        right_internal->page.reset();
        auto root = buffer_pool.allocate_index_internal_page(
            2, left_internal_id);
        if (!root) {
            expect(false, "multilevel root page is allocated");
            return;
        }
        root_page_id = root->page_id;
        root->page.reset();

        const auto write_leaf = [&buffer_pool](
                                    const nessodb::index::LeafPage& contents) {
            auto page = buffer_pool.fetch_index_page_for_write(contents.page_id);
            return page &&
                   nessodb::index::encode_leaf_page(**page, contents).has_value();
        };
        const auto write_internal = [&buffer_pool](
                                        const nessodb::index::InternalPage& contents) {
            auto page = buffer_pool.fetch_index_page_for_write(contents.page_id);
            return page && nessodb::index::encode_internal_page(
                               **page, contents).has_value();
        };
        expect(write_leaf({first_leaf_id, left_internal_id, second_leaf_id,
                           {{low_key, {PageId{1}, SlotId{0}}}}}) &&
                   write_leaf({second_leaf_id, left_internal_id, third_leaf_id,
                               {{separator_key, left_duplicate}}}) &&
                   write_leaf({third_leaf_id, right_internal_id, std::nullopt,
                               {{separator_key, right_duplicate}}}) &&
                   write_internal({left_internal_id, root_page_id, 1,
                                   first_leaf_id,
                                   {{separator_key, second_leaf_id}}}) &&
                   write_internal({right_internal_id, root_page_id, 1,
                                   third_leaf_id, {}}) &&
                   write_internal({root_page_id, std::nullopt, 2,
                                   left_internal_id,
                                   {{separator_key, right_internal_id}}}),
               "a valid two-level internal tree is assembled");

        auto tree = BTree::open(buffer_pool, root_page_id);
        expect(tree.has_value(), "tree with two internal levels is opened");
        if (!tree) {
            return;
        }
        expect(tree->find(separator_key) ==
                   std::vector<RecordId>{left_duplicate, right_duplicate},
               "lookup crosses an internal-parent boundary for duplicate keys");
        expect(tree->insert(separator_key, inserted_duplicate).has_value() &&
                   tree->find(separator_key) ==
                       std::vector<RecordId>{inserted_duplicate, left_duplicate,
                                             right_duplicate},
               "insertion descends with right bias through both internal levels");
        expect(tree->insert(high_key, RecordId{PageId{30}, SlotId{0}})
                       .has_value() &&
                   tree->find(low_key) ==
                       std::vector<RecordId>{{PageId{1}, SlotId{0}}} &&
                   tree->find(high_key) ==
                       std::vector<RecordId>{{PageId{30}, SlotId{0}}},
               "multilevel descent reaches both edge leaves");

        {
            auto page = buffer_pool.fetch_index_page_for_write(right_internal_id);
            if (!page) {
                expect(false, "right internal page is fetched for corruption test");
                return;
            }
            expect(nessodb::index::encode_internal_page(
                       **page,
                       {right_internal_id, root_page_id, 2, third_leaf_id, {}})
                       .has_value(),
                   "child level is changed for the corruption test");
        }
        const auto invalid_level = tree->find(high_key);
        expect(!invalid_level &&
                   std::holds_alternative<BTreeErrorCode>(
                       invalid_level.error()) &&
                   std::get<BTreeErrorCode>(invalid_level.error()) ==
                       BTreeErrorCode::invalid_tree_structure,
               "inconsistent internal levels are rejected during descent");
        expect(write_internal({right_internal_id, root_page_id, 1,
                               third_leaf_id, {}}),
               "valid child level is restored");
        expect(buffer_pool.flush().has_value(),
               "multilevel tree is flushed");
    }

    auto database = nessodb::storage::DatabaseFile::open(path);
    expect(database.has_value(), "multilevel database is reopened");
    if (!database) {
        return;
    }
    nessodb::storage::BufferPool buffer_pool(*database, 1);
    auto tree = BTree::open(buffer_pool, root_page_id);
    expect(tree && tree->find(separator_key) ==
                       std::vector<RecordId>{inserted_duplicate, left_duplicate,
                                             right_duplicate},
           "multilevel navigation survives reopening");
}

void test_split_internal_root() {
    TemporaryDirectory directory;
    const auto path = directory.path() / "internal-root-split.mdb";
    const auto key = text_key(std::string(700, 'k'));
    std::vector<RecordId> records;
    PageId root_page_id;
    EncodedKey deep_key;
    RecordId deep_record;
    std::vector<LeafEntry> deep_entries;

    {
        auto database = nessodb::storage::DatabaseFile::create(path);
        if (!database) {
            expect(false, "internal split database is created");
            return;
        }
        nessodb::storage::BufferPool buffer_pool(*database, 1);
        auto tree = BTree::create(buffer_pool);
        if (!tree) {
            expect(false, "internal split tree is created");
            return;
        }

        bool reached_level_two = false;
        for (std::uint64_t value = 100; value < 140; ++value) {
            const RecordId record_id{PageId{value}, SlotId{0}};
            const auto inserted = tree->insert(key, record_id);
            expect(inserted.has_value(),
                   "entries are inserted while growing the internal root");
            if (!inserted) {
                return;
            }
            records.push_back(record_id);

            auto root_page =
                buffer_pool.fetch_index_page(tree->root_page_id());
            if (!root_page) {
                expect(false, "growing tree root is readable");
                return;
            }
            const auto header = nessodb::storage::decode_page_header(**root_page);
            if (header &&
                header->type == nessodb::storage::PageType::index_internal) {
                const auto root =
                    nessodb::index::decode_internal_page(**root_page);
                if (root && root->level == 2) {
                    reached_level_two = true;
                    break;
                }
            }
        }
        expect(reached_level_two,
               "a full internal root grows the tree to level two");
        if (!reached_level_two) {
            return;
        }
        root_page_id = tree->root_page_id();
        expect(tree->find(key) == records,
               "all duplicate records remain visible after internal root split");

        nessodb::index::InternalPage root_contents;
        {
            auto root_page = buffer_pool.fetch_index_page(root_page_id);
            if (!root_page) {
                expect(false, "new level-two root is readable");
                return;
            }
            auto decoded = nessodb::index::decode_internal_page(**root_page);
            if (!decoded) {
                expect(false, "new level-two root is valid");
                return;
            }
            root_contents = std::move(*decoded);
        }
        expect(root_contents.level == 2 && !root_contents.parent_page_id &&
                   root_contents.entries.size() == 1,
               "new root contains the promoted separator");

        const PageId right_internal_id =
            root_contents.entries.front().right_child;
        nessodb::index::InternalPage right_internal;
        {
            auto page = buffer_pool.fetch_index_page(right_internal_id);
            if (!page) {
                expect(false, "new right internal child is readable");
                return;
            }
            auto decoded = nessodb::index::decode_internal_page(**page);
            if (!decoded) {
                expect(false, "new right internal child is valid");
                return;
            }
            right_internal = std::move(*decoded);
        }
        expect(right_internal.level == 1 &&
                   right_internal.parent_page_id == root_page_id,
               "new right internal child points back to the new root");

        std::vector<PageId> right_children{right_internal.leftmost_child};
        for (const auto& entry : right_internal.entries) {
            right_children.push_back(entry.right_child);
        }
        for (const auto child_id : right_children) {
            auto page = buffer_pool.fetch_index_page(child_id);
            if (!page) {
                expect(false, "reparented right leaf is readable");
                return;
            }
            const auto leaf = nessodb::index::decode_leaf_page(**page);
            expect(leaf && leaf->parent_page_id == right_internal_id,
                   "right internal children receive their new parent link");
        }

        const std::size_t initial_right_entry_count =
            right_internal.entries.size();
        bool split_non_root_leaf = false;
        for (std::uint64_t value = 200; value < 220; ++value) {
            const RecordId record_id{PageId{value}, SlotId{0}};
            const auto inserted = tree->insert(key, record_id);
            expect(inserted.has_value(),
                   "a leaf below a non-root internal node can grow");
            if (!inserted) {
                return;
            }
            records.push_back(record_id);

            auto page = buffer_pool.fetch_index_page(right_internal_id);
            if (!page) {
                expect(false, "non-root internal node remains readable");
                return;
            }
            const auto current =
                nessodb::index::decode_internal_page(**page);
            if (!current) {
                expect(false, "non-root internal node remains valid");
                return;
            }
            if (current->entries.size() > initial_right_entry_count) {
                split_non_root_leaf = true;
                break;
            }
        }
        expect(split_non_root_leaf,
               "a full leaf splits below a non-root internal node");
        expect(tree->find(key) == records,
               "records remain visible after a non-root leaf split");

        const std::size_t initial_root_entry_count =
            root_contents.entries.size();
        bool split_non_root_internal = false;
        for (std::uint64_t value = 300; value < 360; ++value) {
            const RecordId record_id{PageId{value}, SlotId{0}};
            const auto inserted = tree->insert(key, record_id);
            expect(inserted.has_value(),
                   "a non-root internal node can continue growing");
            if (!inserted) {
                return;
            }
            records.push_back(record_id);

            auto page = buffer_pool.fetch_index_page(root_page_id);
            if (!page) {
                expect(false, "level-two root remains readable");
                return;
            }
            const auto current =
                nessodb::index::decode_internal_page(**page);
            if (!current) {
                expect(false, "level-two root remains valid");
                return;
            }
            if (current->entries.size() > initial_root_entry_count) {
                split_non_root_internal = true;
                break;
            }
        }
        expect(split_non_root_internal,
               "a full non-root internal node propagates its split");
        expect(tree->find(key) == records,
               "records remain visible after internal split propagation");

        bool reached_level_three = false;
        for (std::uint64_t value = 400; value < 500; ++value) {
            const RecordId record_id{PageId{value}, SlotId{0}};
            const auto inserted = tree->insert(key, record_id);
            expect(inserted.has_value(),
                   "the level-two root can continue growing");
            if (!inserted) {
                return;
            }
            records.push_back(record_id);
            if (tree->root_page_id() != root_page_id) {
                reached_level_three = true;
                break;
            }
        }
        expect(reached_level_three,
               "a full level-two root grows the tree to level three");
        if (!reached_level_three) {
            return;
        }
        root_page_id = tree->root_page_id();
        std::size_t initial_level_four_entry_count = 0;
        {
            auto page = buffer_pool.fetch_index_page(root_page_id);
            if (!page) {
                expect(false, "new level-three root is readable");
                return;
            }
            const auto current =
                nessodb::index::decode_internal_page(**page);
            expect(current && current->level == 3 &&
                       !current->parent_page_id,
                   "new root has level three and no parent");
        }
        expect(tree->find(key) == records,
               "records remain visible after level-three growth");

        PageId right_level_two_id;
        std::size_t initial_level_two_entry_count = 0;
        {
            auto page = buffer_pool.fetch_index_page(root_page_id);
            if (!page) {
                expect(false, "level-three root remains readable");
                return;
            }
            const auto root = nessodb::index::decode_internal_page(**page);
            if (!root) {
                expect(false, "level-three root remains valid");
                return;
            }
            right_level_two_id = root->entries.empty()
                                     ? root->leftmost_child
                                     : root->entries.back().right_child;
        }
        {
            auto page = buffer_pool.fetch_index_page(right_level_two_id);
            if (!page) {
                expect(false, "right level-two node is readable");
                return;
            }
            const auto level_two =
                nessodb::index::decode_internal_page(**page);
            if (!level_two) {
                expect(false, "right level-two node is valid");
                return;
            }
            initial_level_two_entry_count = level_two->entries.size();
        }

        bool propagated_inside_level_three = false;
        for (std::uint64_t value = 500; value < 600; ++value) {
            const RecordId record_id{PageId{value}, SlotId{0}};
            const auto inserted = tree->insert(key, record_id);
            expect(inserted.has_value(),
                   "a level-three tree can continue growing");
            if (!inserted) {
                return;
            }
            records.push_back(record_id);

            auto page = buffer_pool.fetch_index_page(right_level_two_id);
            if (!page) {
                expect(false, "right level-two node remains readable");
                return;
            }
            const auto level_two =
                nessodb::index::decode_internal_page(**page);
            if (!level_two) {
                expect(false, "right level-two node remains valid");
                return;
            }
            if (level_two->entries.size() >
                initial_level_two_entry_count) {
                propagated_inside_level_three = true;
                break;
            }
        }
        expect(propagated_inside_level_three,
               "a level-one split propagates inside a level-three tree");
        expect(tree->find(key) == records,
               "records survive propagation inside a level-three tree");

        std::size_t initial_level_three_entry_count = 0;
        {
            auto page = buffer_pool.fetch_index_page(root_page_id);
            if (!page) {
                expect(false, "level-three root is readable before growth");
                return;
            }
            const auto root = nessodb::index::decode_internal_page(**page);
            if (!root) {
                expect(false, "level-three root is valid before growth");
                return;
            }
            initial_level_three_entry_count = root->entries.size();
        }

        bool propagated_level_two_split = false;
        for (std::uint64_t value = 600; value < 850; ++value) {
            const RecordId record_id{PageId{value}, SlotId{0}};
            const auto inserted = tree->insert(key, record_id);
            expect(inserted.has_value(),
                   "a level-two child can continue growing");
            if (!inserted) {
                return;
            }
            records.push_back(record_id);

            auto page = buffer_pool.fetch_index_page(root_page_id);
            if (!page) {
                expect(false, "level-three root remains readable");
                return;
            }
            const auto root = nessodb::index::decode_internal_page(**page);
            if (!root) {
                expect(false, "level-three root remains valid");
                return;
            }
            if (root->entries.size() >
                initial_level_three_entry_count) {
                propagated_level_two_split = true;
                break;
            }
        }
        expect(propagated_level_two_split,
               "a level-two split propagates to the level-three root");
        expect(tree->find(key) == records,
               "records survive level-two split propagation");

        bool reached_level_four = false;
        for (std::uint64_t value = 850; value < 2000; ++value) {
            const RecordId record_id{PageId{value}, SlotId{0}};
            const auto inserted = tree->insert(key, record_id);
            expect(inserted.has_value(),
                   "the level-three root can continue growing");
            if (!inserted) {
                return;
            }
            records.push_back(record_id);
            if (tree->root_page_id() != root_page_id) {
                reached_level_four = true;
                break;
            }
        }
        expect(reached_level_four,
               "a full level-three root grows the tree to level four");
        if (!reached_level_four) {
            return;
        }
        root_page_id = tree->root_page_id();
        {
            auto page = buffer_pool.fetch_index_page(root_page_id);
            if (!page) {
                expect(false, "new level-four root is readable");
                return;
            }
            const auto root = nessodb::index::decode_internal_page(**page);
            expect(root && root->level == 4 && !root->parent_page_id,
                   "new root has level four and no parent");
            if (root) {
                initial_level_four_entry_count = root->entries.size();
            }
        }
        expect(tree->find(key) == records,
               "records remain visible after level-four growth");

        bool propagated_inside_level_four = false;
        for (std::uint64_t value = 2000; value < 4000; ++value) {
            deep_key = text_key(
                std::string(690, 'z') + std::to_string(value));
            deep_record = RecordId{PageId{value}, SlotId{1}};
            const auto inserted =
                tree->insert(deep_key, deep_record);
            expect(inserted.has_value(),
                   "a level-four tree can continue growing");
            if (!inserted) {
                return;
            }
            deep_entries.push_back(LeafEntry{deep_key, deep_record});

            auto page = buffer_pool.fetch_index_page(root_page_id);
            if (!page) {
                expect(false, "level-four root remains readable");
                return;
            }
            const auto root = nessodb::index::decode_internal_page(**page);
            if (!root) {
                expect(false, "level-four root remains valid");
                return;
            }
            if (root->entries.size() >
                initial_level_four_entry_count) {
                propagated_inside_level_four = true;
                break;
            }
        }
        expect(propagated_inside_level_four,
               "splits propagate through a level-four tree");
        expect(tree->find(deep_key) ==
                   std::vector<RecordId>{deep_record},
               "deep recursive propagation preserves inserted records");
        expect(buffer_pool.flush().has_value(),
               "tree with split internal root is flushed");
    }

    auto database = nessodb::storage::DatabaseFile::open(path);
    expect(database.has_value(), "internal split database is reopened");
    if (!database) {
        return;
    }
    nessodb::storage::BufferPool buffer_pool(*database, 1);
    auto tree = BTree::open(buffer_pool, root_page_id);
    expect(tree && tree->find(key) == records,
           "automatically grown multilevel tree survives reopening");
    expect(tree && tree->find(deep_key) ==
                       std::vector<RecordId>{deep_record},
           "deep recursive propagation survives reopening");
    if (!tree) {
        return;
    }

    for (const auto& entry : deep_entries) {
        const auto erased = tree->erase(entry.key, entry.record_id);
        expect(erased.has_value(),
               "deep entry is erased before shrinking the duplicate tree");
        if (!erased) {
            return;
        }
    }
    for (std::size_t index = 0; index + 1 < records.size(); ++index) {
        const auto erased = tree->erase(key, records[index]);
        expect(erased.has_value(),
               "multilevel duplicate entry is erased while shrinking");
        if (!erased) {
            return;
        }
    }
    expect(tree->find(key) ==
               std::vector<RecordId>{records.back()},
           "recursive internal rebalancing preserves the final entry");
    auto collapsed_root =
        buffer_pool.fetch_index_page(tree->root_page_id());
    expect(collapsed_root.has_value(),
           "recursively collapsed root is readable");
    if (collapsed_root) {
        const auto leaf =
            nessodb::index::decode_leaf_page(**collapsed_root);
        expect(leaf && !leaf->parent_page_id && !leaf->next_page_id,
               "recursive internal merges collapse the tree to one leaf");
    }
    expect(buffer_pool.flush().has_value(),
           "recursively collapsed tree is flushed");
}

}  // namespace

int main() {
    test_single_leaf_operations();
    test_reopen_tree();
    test_range_scan();
    test_erase_across_leaf_chain();
    test_leaf_redistribution_after_erase();
    test_leaf_merge_after_erase();
    test_split_leaf_root();
    test_multilevel_navigation();
    test_split_internal_root();

    if (failures != 0) {
        std::cerr << failures << " B+ tree assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
