#include "index/btree_page.hpp"

#include "index/key_codec.hpp"
#include "storage/page/checksum.hpp"
#include "storage/page/codec.hpp"
#include "storage/page/page.hpp"
#include "storage/page/page_header.hpp"
#include "types/value.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using nessodb::common::PageId;
using nessodb::index::IndexPageError;
using nessodb::storage::PageBuffer;
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

nessodb::index::EncodedKey integer_key(std::int64_t value) {
    const std::vector<Value> values{value};
    return nessodb::index::encode_key(values);
}

nessodb::index::EncodedKey text_key(std::string value) {
    const std::vector<Value> values{std::move(value)};
    return nessodb::index::encode_key(values);
}

nessodb::index::LeafPage sample_leaf() {
    return nessodb::index::LeafPage{
        PageId{10},
        PageId{20},
        PageId{11},
        {
            {integer_key(1), RecordId{PageId{100}, SlotId{1}}},
            {integer_key(1), RecordId{PageId{100}, SlotId{2}}},
            {integer_key(2), RecordId{PageId{101}, SlotId{0}}},
        },
    };
}

nessodb::index::InternalPage sample_internal() {
    return nessodb::index::InternalPage{
        PageId{20},
        std::nullopt,
        1,
        PageId{10},
        {
            {integer_key(50), PageId{11}},
            {integer_key(100), PageId{12}},
        },
    };
}

void test_leaf_round_trip() {
    PageBuffer buffer{};
    const auto expected = sample_leaf();
    expect(nessodb::index::encode_leaf_page(buffer, expected).has_value(),
           "leaf page is encoded");

    const auto header = nessodb::storage::decode_page_header(buffer);
    expect(header && header->type == nessodb::storage::PageType::index_leaf &&
               header->page_id == expected.page_id,
           "leaf page uses the common typed page header");
    expect(nessodb::storage::read_u16(buffer, 32) ==
               nessodb::index::index_page_format_version &&
               nessodb::storage::read_u16(buffer, 34) == 3 &&
               nessodb::storage::read_u16(buffer, 36) ==
                   nessodb::index::index_page_header_size +
                       3 * nessodb::index::index_slot_size,
           "leaf page stores its version and slot boundaries");

    const auto decoded = nessodb::index::decode_leaf_page(buffer);
    expect(decoded && *decoded == expected,
           "leaf metadata, duplicate keys, and record IDs round-trip");

    expect(nessodb::storage::update_page_checksum(buffer).has_value() &&
               nessodb::storage::verify_page_checksum(buffer).has_value(),
           "index leaf participates in common page checksums");
}

void test_internal_round_trip() {
    PageBuffer buffer{};
    const auto expected = sample_internal();
    expect(nessodb::index::encode_internal_page(buffer, expected).has_value(),
           "internal page is encoded");

    const auto header = nessodb::storage::decode_page_header(buffer);
    expect(header &&
               header->type == nessodb::storage::PageType::index_internal,
           "internal page has a distinct common page type");
    const auto decoded = nessodb::index::decode_internal_page(buffer);
    expect(decoded && *decoded == expected,
           "internal level, children, and separators round-trip");

    PageBuffer empty_buffer{};
    auto empty = expected;
    empty.entries.clear();
    expect(nessodb::index::encode_internal_page(empty_buffer, empty).has_value() &&
               nessodb::index::decode_internal_page(empty_buffer) == empty,
           "an internal page may temporarily contain only its leftmost child");

    PageBuffer duplicate_buffer{};
    auto duplicate_separators = expected;
    duplicate_separators.entries[1].key =
        duplicate_separators.entries[0].key;
    expect(nessodb::index::encode_internal_page(
               duplicate_buffer, duplicate_separators).has_value() &&
               nessodb::index::decode_internal_page(duplicate_buffer) ==
                   duplicate_separators,
           "equal separators may route duplicate keys to distinct children");
}

void test_encode_validation() {
    PageBuffer buffer{};

    auto leaf = sample_leaf();
    leaf.page_id = PageId{0};
    expect(nessodb::index::encode_leaf_page(buffer, leaf) ==
               std::unexpected(IndexPageError::invalid_page_id),
           "page zero cannot contain an index leaf");

    leaf = sample_leaf();
    leaf.parent_page_id = leaf.page_id;
    expect(nessodb::index::encode_leaf_page(buffer, leaf) ==
               std::unexpected(IndexPageError::invalid_parent_page_id),
           "leaf cannot be its own parent");

    leaf = sample_leaf();
    leaf.next_page_id = leaf.page_id;
    expect(nessodb::index::encode_leaf_page(buffer, leaf) ==
               std::unexpected(IndexPageError::invalid_link_page_id),
           "leaf cannot link to itself");

    leaf = sample_leaf();
    std::swap(leaf.entries[0], leaf.entries[2]);
    expect(nessodb::index::encode_leaf_page(buffer, leaf) ==
               std::unexpected(IndexPageError::entries_not_sorted),
           "leaf entries must be ordered");

    leaf = sample_leaf();
    leaf.entries[1].record_id = leaf.entries[0].record_id;
    expect(nessodb::index::encode_leaf_page(buffer, leaf) ==
               std::unexpected(IndexPageError::entries_not_sorted),
           "duplicate key and record pairs are rejected");

    leaf = sample_leaf();
    leaf.entries[0].key = {std::byte{99}};
    expect(nessodb::index::encode_leaf_page(buffer, leaf) ==
               std::unexpected(IndexPageError::invalid_key),
           "malformed encoded keys are rejected");

    leaf = nessodb::index::LeafPage{
        PageId{10}, std::nullopt, std::nullopt,
        {{text_key(std::string(5000, 'x')),
          RecordId{PageId{100}, SlotId{0}}}}};
    expect(nessodb::index::encode_leaf_page(buffer, leaf) ==
               std::unexpected(IndexPageError::page_full),
           "entry larger than a page is rejected");

    auto internal = sample_internal();
    internal.level = 0;
    expect(nessodb::index::encode_internal_page(buffer, internal) ==
               std::unexpected(IndexPageError::invalid_level),
           "internal page level must be positive");

    internal = sample_internal();
    std::swap(internal.entries[0], internal.entries[1]);
    expect(nessodb::index::encode_internal_page(buffer, internal) ==
               std::unexpected(IndexPageError::entries_not_sorted),
           "internal separators cannot be stored in descending order");

    internal = sample_internal();
    internal.entries[1].right_child = internal.entries[0].right_child;
    expect(nessodb::index::encode_internal_page(buffer, internal) ==
               std::unexpected(IndexPageError::invalid_link_page_id),
           "internal child links must be distinct");
}

void test_decode_validation() {
    PageBuffer buffer{};
    expect(nessodb::index::encode_leaf_page(buffer, sample_leaf()).has_value(),
           "corruption fixture leaf is encoded");

    expect(nessodb::index::decode_internal_page(buffer) ==
               std::unexpected(IndexPageError::wrong_page_type),
           "leaf cannot be decoded as an internal page");

    auto corrupted = buffer;
    expect(nessodb::storage::write_u16(corrupted, 32, 2).has_value(),
           "index version is corrupted");
    expect(nessodb::index::decode_leaf_page(corrupted) ==
               std::unexpected(IndexPageError::unsupported_version),
           "unknown index page version is rejected");

    corrupted = buffer;
    expect(nessodb::storage::write_u16(corrupted, 36, 64).has_value(),
           "slot boundary is corrupted");
    expect(nessodb::index::decode_leaf_page(corrupted) ==
               std::unexpected(IndexPageError::corrupted_slot_directory),
           "incorrect slot boundary is rejected");

    corrupted = buffer;
    expect(nessodb::storage::write_u16(corrupted, 56, 1).has_value(),
           "leaf level is corrupted");
    expect(nessodb::index::decode_leaf_page(corrupted) ==
               std::unexpected(IndexPageError::invalid_level),
           "nonzero leaf level is rejected");

    corrupted = buffer;
    expect(nessodb::storage::write_u16(corrupted, 58, 1).has_value(),
           "reserved index bytes are corrupted");
    expect(nessodb::index::decode_leaf_page(corrupted) ==
               std::unexpected(IndexPageError::reserved_bytes_nonzero),
           "nonzero reserved bytes are rejected");

    corrupted = buffer;
    const auto first_offset = *nessodb::storage::read_u16(corrupted, 64);
    corrupted[first_offset] = std::byte{99};
    expect(nessodb::index::decode_leaf_page(corrupted) ==
               std::unexpected(IndexPageError::invalid_key),
           "corrupted key encoding is rejected");

    corrupted = buffer;
    expect(nessodb::storage::write_u16(corrupted, 68, first_offset).has_value(),
           "two slots are made to overlap");
    expect(nessodb::index::decode_leaf_page(corrupted) ==
               std::unexpected(IndexPageError::corrupted_slot_directory),
           "overlapping entry payloads are rejected");
}

}  // namespace

int main() {
    test_leaf_round_trip();
    test_internal_round_trip();
    test_encode_validation();
    test_decode_validation();

    if (failures != 0) {
        std::cerr << failures << " B+ tree page assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
