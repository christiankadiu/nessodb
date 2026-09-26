#include "common/page_id.hpp"
#include "storage/page/checksum.hpp"
#include "storage/page/codec.hpp"
#include "storage/page/database_header.hpp"
#include "storage/page/page.hpp"
#include "storage/page/page_header.hpp"
#include "storage/page/slotted_page.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

void test_page_identifier() {
    const nessodb::common::PageId invalid;
    const nessodb::common::PageId header_page{0};

    expect(!invalid.is_valid(), "default page identifier is invalid");
    expect(header_page.is_valid(), "page zero is a valid identifier");
    expect(header_page != nessodb::common::PageId{1}, "different pages have different IDs");
}

void test_page_buffer() {
    nessodb::storage::PageBuffer page{};

    expect(page.size() == nessodb::storage::page_size, "page has the fixed configured size");
    expect(page.front() == std::byte{0} && page.back() == std::byte{0},
           "value-initialized page is zeroed");
}

void test_integer_codec() {
    nessodb::storage::PageBuffer page{};

    const auto wrote_u16 = nessodb::storage::write_u16(page, 0, 0x1234);
    const auto wrote_u32 = nessodb::storage::write_u32(page, 2, 0x89abcdef);
    const auto wrote_u64 =
        nessodb::storage::write_u64(page, 6, 0x0123456789abcdefULL);
    expect(wrote_u16 && wrote_u32 && wrote_u64, "integers are written within page bounds");

    expect(page[0] == std::byte{0x34} && page[1] == std::byte{0x12},
           "integer codec uses little-endian byte order");
    expect(nessodb::storage::read_u16(page, 0) == 0x1234, "16-bit integer round-trips");
    expect(nessodb::storage::read_u32(page, 2) == 0x89abcdef, "32-bit integer round-trips");
    expect(nessodb::storage::read_u64(page, 6) == 0x0123456789abcdefULL,
           "64-bit integer round-trips");
}

void test_codec_bounds() {
    nessodb::storage::PageBuffer page{};
    const std::size_t last_u64_offset = page.size() - sizeof(std::uint64_t);

    expect(nessodb::storage::write_u64(page, last_u64_offset, 42).has_value(),
           "integer fits at final valid offset");
    const auto invalid_write = nessodb::storage::write_u64(page, last_u64_offset + 1, 7);
    expect(!invalid_write && invalid_write.error() == nessodb::storage::CodecError::out_of_bounds,
           "out-of-bounds write is rejected");

    const auto invalid_read = nessodb::storage::read_u32(page, page.size());
    expect(!invalid_read && invalid_read.error() == nessodb::storage::CodecError::out_of_bounds,
           "out-of-bounds read is rejected");
    expect(nessodb::storage::read_u64(page, last_u64_offset) == 42,
           "rejected write does not modify the page");
}

void test_page_header_round_trip() {
    nessodb::storage::PageBuffer page{};
    const nessodb::storage::PageHeader expected{
        nessodb::storage::PageType::heap,
        nessodb::common::PageId{42},
        17,
        0x12345678,
    };

    const auto encoded = nessodb::storage::encode_page_header(page, expected);
    expect(encoded.has_value(), "valid page header is encoded");
    expect(page[0] == std::byte{'M'} && page[1] == std::byte{'D'} &&
               page[2] == std::byte{'B'} && page[3] == std::byte{'P'},
           "page magic has readable byte order");

    const auto decoded = nessodb::storage::decode_page_header(page);
    expect(decoded.has_value(), "valid page header is decoded");
    if (decoded) {
        expect(*decoded == expected, "page header round-trips exactly");
    }
}

void test_page_header_validation() {
    nessodb::storage::PageBuffer page{};
    const nessodb::storage::PageHeader header{
        nessodb::storage::PageType::database_header,
        nessodb::common::PageId{0},
    };
    const auto encoded = nessodb::storage::encode_page_header(page, header);
    expect(encoded.has_value(), "database header page zero is encoded");

    page[0] = std::byte{0};
    const auto invalid_magic = nessodb::storage::decode_page_header(page);
    expect(!invalid_magic &&
               invalid_magic.error() == nessodb::storage::PageHeaderError::invalid_magic,
           "invalid page magic is rejected");

    const auto restored = nessodb::storage::encode_page_header(page, header);
    expect(restored.has_value(), "page header is restored after corruption test");
    const auto reserved = nessodb::storage::write_u32(page, 28, 1);
    expect(reserved.has_value(), "reserved bytes are changed for validation test");
    const auto invalid_reserved = nessodb::storage::decode_page_header(page);
    expect(!invalid_reserved && invalid_reserved.error() ==
                                    nessodb::storage::PageHeaderError::reserved_bytes_nonzero,
           "nonzero reserved bytes are rejected");

    std::span<std::byte> short_page{page.data(), nessodb::storage::page_header_size - 1};
    const auto too_small = nessodb::storage::encode_page_header(short_page, header);
    expect(!too_small && too_small.error() == nessodb::storage::PageHeaderError::page_too_small,
           "short page header destination is rejected");

    const nessodb::storage::PageHeader heap_at_zero{
        nessodb::storage::PageType::heap,
        nessodb::common::PageId{0},
    };
    const auto invalid_page_id = nessodb::storage::encode_page_header(page, heap_at_zero);
    expect(!invalid_page_id &&
               invalid_page_id.error() == nessodb::storage::PageHeaderError::invalid_page_id,
           "page zero is reserved for the database header");
}

void test_empty_slotted_page() {
    nessodb::storage::PageBuffer page{};
    const auto initialized =
        nessodb::storage::SlottedPage::initialize(page, nessodb::common::PageId{1});
    expect(initialized.has_value(), "heap page is initialized");
    if (!initialized) {
        return;
    }

    expect(initialized->page_id() == nessodb::common::PageId{1},
           "heap page preserves its identifier");
    expect(initialized->slot_count() == 0, "new heap page has no slots");
    expect(!initialized->next_page_id(), "new heap page has no successor");
    expect(initialized->free_space() ==
               nessodb::storage::page_size - nessodb::storage::heap_page_header_size,
           "new heap page exposes all payload space");

    const auto reopened = nessodb::storage::SlottedPage::open(page);
    expect(reopened.has_value(), "initialized heap page can be reopened");
}

void test_slotted_page_links() {
    nessodb::storage::PageBuffer page{};
    auto slotted_page =
        nessodb::storage::SlottedPage::initialize(page, nessodb::common::PageId{1});
    expect(slotted_page.has_value(), "linked heap page is initialized");
    if (!slotted_page) {
        return;
    }

    expect(slotted_page->set_next_page_id(nessodb::common::PageId{2}).has_value(),
           "heap page successor is set");
    expect(slotted_page->next_page_id() == nessodb::common::PageId{2},
           "heap page exposes its successor");

    auto reopened = nessodb::storage::SlottedPage::open(page);
    expect(reopened && reopened->next_page_id() == nessodb::common::PageId{2},
           "heap page successor survives reopening");
    expect(slotted_page->set_next_page_id(std::nullopt).has_value(),
           "heap page successor can be cleared");

    const auto self_link = slotted_page->set_next_page_id(nessodb::common::PageId{1});
    expect(!self_link &&
               self_link.error() == nessodb::storage::SlottedPageError::invalid_next_page_id,
           "heap page cannot link to itself");
    const auto header_link = slotted_page->set_next_page_id(nessodb::common::PageId{0});
    expect(!header_link &&
               header_link.error() == nessodb::storage::SlottedPageError::invalid_next_page_id,
           "heap page cannot link to database header");
}

void test_slotted_page_validation() {
    nessodb::storage::PageBuffer page{};
    const auto uninitialized = nessodb::storage::SlottedPage::open(page);
    expect(!uninitialized &&
               uninitialized.error() == nessodb::storage::SlottedPageError::invalid_page_header,
           "uninitialized page is rejected");

    const auto initialized =
        nessodb::storage::SlottedPage::initialize(page, nessodb::common::PageId{1});
    expect(initialized.has_value(), "validation test heap page is initialized");
    const auto corrupted = nessodb::storage::write_u16(page, 34, 39);
    expect(corrupted.has_value(), "free-space boundary is changed for validation test");
    const auto invalid_directory = nessodb::storage::SlottedPage::open(page);
    expect(!invalid_directory && invalid_directory.error() ==
                                     nessodb::storage::SlottedPageError::corrupted_slot_directory,
           "overlapping slot directory is rejected");

    expect(nessodb::storage::SlottedPage::initialize(
               page, nessodb::common::PageId{1}).has_value(),
           "validation heap page is reinitialized");
    expect(nessodb::storage::write_u16(page, 36, 2).has_value(),
           "heap format version is changed for validation test");
    const auto unknown_heap_version = nessodb::storage::SlottedPage::open(page);
    expect(!unknown_heap_version &&
               unknown_heap_version.error() ==
                   nessodb::storage::SlottedPageError::unsupported_heap_page_version,
           "unknown heap page format version is rejected");

    expect(nessodb::storage::SlottedPage::initialize(
               page, nessodb::common::PageId{1}).has_value(),
           "validation heap page is reinitialized again");
    expect(nessodb::storage::write_u64(
               page, 40, nessodb::common::PageId::invalid_value).has_value(),
           "invalid successor is written for validation test");
    const auto invalid_successor = nessodb::storage::SlottedPage::open(page);
    expect(!invalid_successor &&
               invalid_successor.error() ==
                   nessodb::storage::SlottedPageError::invalid_next_page_id,
           "invalid heap page successor is rejected");

    std::span<std::byte> short_page{page.data(), page.size() - 1};
    const auto invalid_size = nessodb::storage::SlottedPage::open(short_page);
    expect(!invalid_size &&
               invalid_size.error() == nessodb::storage::SlottedPageError::invalid_page_size,
           "nonstandard page size is rejected");
}

void test_slotted_page_records() {
    nessodb::storage::PageBuffer page{};
    auto slotted_page =
        nessodb::storage::SlottedPage::initialize(page, nessodb::common::PageId{1});
    expect(slotted_page.has_value(), "record test heap page is initialized");
    if (!slotted_page) {
        return;
    }

    constexpr std::array first_record{std::byte{1}, std::byte{2}, std::byte{3}};
    constexpr std::array second_record{std::byte{4}, std::byte{5}};
    const auto first_slot = slotted_page->insert(first_record);
    const auto second_slot = slotted_page->insert(second_record);
    expect(first_slot == nessodb::storage::SlotId{0}, "first record receives slot zero");
    expect(second_slot == nessodb::storage::SlotId{1}, "second record receives slot one");
    expect(slotted_page->slot_count() == 2, "record insertion increments slot count");

    const auto first_read = slotted_page->read(*first_slot);
    const auto second_read = slotted_page->read(*second_slot);
    expect(first_read && std::ranges::equal(*first_read, first_record),
           "first record round-trips");
    expect(second_read && std::ranges::equal(*second_read, second_record),
           "second record round-trips");

    auto reopened = nessodb::storage::SlottedPage::open(page);
    expect(reopened.has_value(), "heap page with records can be reopened");
    if (reopened) {
        const auto reopened_record = reopened->read(nessodb::storage::SlotId{1});
        expect(reopened_record && std::ranges::equal(*reopened_record, second_record),
               "record remains readable after reopening");
    }

    const auto invalid = slotted_page->read(nessodb::storage::SlotId{2});
    expect(!invalid && invalid.error() == nessodb::storage::SlottedPageError::invalid_slot,
           "unknown slot is rejected");
    const auto empty = slotted_page->insert({});
    expect(!empty && empty.error() == nessodb::storage::SlottedPageError::empty_record,
           "empty record is rejected");
}

void test_slotted_page_deletion() {
    nessodb::storage::PageBuffer page{};
    auto slotted_page =
        nessodb::storage::SlottedPage::initialize(page, nessodb::common::PageId{1});
    expect(slotted_page.has_value(), "deletion test heap page is initialized");
    if (!slotted_page) {
        return;
    }

    constexpr std::array first_record{std::byte{1}, std::byte{2}, std::byte{3}};
    constexpr std::array second_record{std::byte{4}, std::byte{5}};
    constexpr std::array third_record{
        std::byte{6}, std::byte{7}, std::byte{8}, std::byte{9}};
    const auto first_slot = slotted_page->insert(first_record);
    const auto second_slot = slotted_page->insert(second_record);
    const auto third_slot = slotted_page->insert(third_record);
    expect(first_slot && second_slot && third_slot,
           "deletion test records are inserted");
    if (!first_slot || !second_slot || !third_slot) {
        return;
    }

    const std::size_t free_space_before = slotted_page->free_space();
    expect(slotted_page->erase(*second_slot).has_value(),
           "middle record is deleted");
    expect(slotted_page->slot_count() == 3,
           "deletion preserves stable slot identifiers");
    expect(slotted_page->free_space() == free_space_before + second_record.size(),
           "deletion releases record payload space");

    const auto deleted = slotted_page->read(*second_slot);
    expect(!deleted &&
               deleted.error() == nessodb::storage::SlottedPageError::deleted_slot,
           "deleted slot cannot be read");
    expect(std::ranges::equal(*slotted_page->read(*first_slot), first_record) &&
               std::ranges::equal(*slotted_page->read(*third_slot), third_record),
           "deletion preserves surrounding records");

    expect(slotted_page->restore(*second_slot, second_record).has_value(),
           "a deleted record is restored to its original slot");
    expect(std::ranges::equal(*slotted_page->read(*second_slot), second_record),
           "restored record contents are readable");
    const auto occupied = slotted_page->restore(*second_slot, second_record);
    expect(!occupied &&
               occupied.error() == nessodb::storage::SlottedPageError::occupied_slot,
           "restoring an occupied slot is rejected");
    expect(slotted_page->erase(*second_slot).has_value(),
           "restored record can be deleted again");

    auto reopened = nessodb::storage::SlottedPage::open(page);
    expect(reopened.has_value(), "page with a deleted slot can be reopened");
    if (!reopened) {
        return;
    }

    constexpr std::array replacement{
        std::byte{10}, std::byte{11}, std::byte{12}};
    const auto replacement_slot = reopened->insert(replacement);
    expect(replacement_slot == second_slot,
           "insertion reuses a deleted slot identifier");
    expect(replacement_slot &&
               std::ranges::equal(*reopened->read(*replacement_slot), replacement),
           "replacement record is readable");

    expect(reopened->erase(*replacement_slot).has_value(),
           "replacement record can be deleted");
    const auto repeated = reopened->erase(*replacement_slot);
    expect(!repeated &&
               repeated.error() == nessodb::storage::SlottedPageError::deleted_slot,
           "deleting an empty slot is rejected");
    const auto invalid = reopened->erase(nessodb::storage::SlotId{3});
    expect(!invalid &&
               invalid.error() == nessodb::storage::SlottedPageError::invalid_slot,
           "deleting an unknown slot is rejected");
}

void test_slotted_page_update() {
    nessodb::storage::PageBuffer page{};
    auto slotted_page =
        nessodb::storage::SlottedPage::initialize(page, nessodb::common::PageId{1});
    expect(slotted_page.has_value(), "update test heap page is initialized");
    if (!slotted_page) {
        return;
    }

    constexpr std::array first_record{std::byte{1}, std::byte{2}};
    constexpr std::array second_record{
        std::byte{3}, std::byte{4}, std::byte{5}};
    constexpr std::array third_record{std::byte{6}, std::byte{7}};
    const auto first_slot = slotted_page->insert(first_record);
    const auto second_slot = slotted_page->insert(second_record);
    const auto third_slot = slotted_page->insert(third_record);
    expect(first_slot && second_slot && third_slot,
           "update test records are inserted");
    if (!first_slot || !second_slot || !third_slot) {
        return;
    }

    const std::size_t initial_free_space = slotted_page->free_space();
    constexpr std::array same_size{
        std::byte{8}, std::byte{9}, std::byte{10}};
    expect(slotted_page->update(*second_slot, same_size).has_value(),
           "record is updated without changing its size");
    expect(slotted_page->free_space() == initial_free_space &&
               std::ranges::equal(*slotted_page->read(*second_slot), same_size),
           "same-size update replaces only the record contents");

    constexpr std::array smaller{std::byte{11}};
    expect(slotted_page->update(*second_slot, smaller).has_value(),
           "record is replaced by a smaller record");
    expect(slotted_page->free_space() == initial_free_space + 2,
           "smaller replacement releases payload space");

    constexpr std::array larger{
        std::byte{12}, std::byte{13}, std::byte{14}, std::byte{15},
        std::byte{16}};
    expect(slotted_page->update(*second_slot, larger).has_value(),
           "record is replaced by a larger record");
    expect(slotted_page->free_space() == initial_free_space - 2,
           "larger replacement consumes payload space");
    expect(slotted_page->slot_count() == 3 &&
               std::ranges::equal(*slotted_page->read(*first_slot), first_record) &&
               std::ranges::equal(*slotted_page->read(*second_slot), larger) &&
               std::ranges::equal(*slotted_page->read(*third_slot), third_record),
           "updates preserve the slot identifier and surrounding records");

    auto reopened = nessodb::storage::SlottedPage::open(page);
    expect(reopened && std::ranges::equal(*reopened->read(*second_slot), larger),
           "page remains valid after record updates");

    const auto empty = slotted_page->update(*second_slot, {});
    expect(!empty && empty.error() == nessodb::storage::SlottedPageError::empty_record,
           "empty replacement is rejected");
    const auto invalid =
        slotted_page->update(nessodb::storage::SlotId{3}, first_record);
    expect(!invalid &&
               invalid.error() == nessodb::storage::SlottedPageError::invalid_slot,
           "updating an unknown slot is rejected");

    expect(slotted_page->erase(*second_slot).has_value(),
           "updated record can be deleted");
    const auto deleted = slotted_page->update(*second_slot, first_record);
    expect(!deleted &&
               deleted.error() == nessodb::storage::SlottedPageError::deleted_slot,
           "updating a deleted slot is rejected");
}

void test_slotted_page_capacity() {
    nessodb::storage::PageBuffer page{};
    auto slotted_page =
        nessodb::storage::SlottedPage::initialize(page, nessodb::common::PageId{1});
    expect(slotted_page.has_value(), "capacity test heap page is initialized");
    if (!slotted_page) {
        return;
    }

    std::vector<std::byte> record(slotted_page->free_space() - nessodb::storage::slot_entry_size,
                                  std::byte{7});
    const auto inserted = slotted_page->insert(record);
    expect(inserted.has_value(), "record can consume all available payload space");
    expect(slotted_page->free_space() == 0, "full heap page has no free space");

    const auto before_failure = page;
    constexpr std::array extra_record{std::byte{1}};
    const auto full = slotted_page->insert(extra_record);
    expect(!full && full.error() == nessodb::storage::SlottedPageError::page_full,
           "record that does not fit is rejected");
    expect(page == before_failure, "failed insertion leaves page unchanged");

    expect(slotted_page->erase(*inserted).has_value(),
           "full-page record can be deleted");
    const auto reinserted = slotted_page->insert(record);
    expect(reinserted == inserted && slotted_page->free_space() == 0,
           "deleted slot allows full-page record to be reinserted");

    std::vector<std::byte> oversized(record.size() + 1, std::byte{8});
    const auto before_update_failure = page;
    const auto update = slotted_page->update(*reinserted, oversized);
    expect(!update && update.error() == nessodb::storage::SlottedPageError::page_full,
           "replacement that exceeds page capacity is rejected");
    expect(page == before_update_failure,
           "failed replacement leaves page unchanged");
}

void test_crc32c() {
    constexpr std::string_view input{"123456789"};
    const auto bytes = std::as_bytes(std::span{input});
    expect(nessodb::storage::crc32c(bytes) == 0xe3069283,
           "CRC32C matches the standard check value");
}

void test_page_checksum() {
    nessodb::storage::PageBuffer page{};
    auto slotted_page =
        nessodb::storage::SlottedPage::initialize(page, nessodb::common::PageId{1});
    expect(slotted_page.has_value(), "checksum test heap page is initialized");

    const auto updated = nessodb::storage::update_page_checksum(page);
    expect(updated.has_value(), "page checksum is updated");
    expect(nessodb::storage::verify_page_checksum(page).has_value(),
           "unchanged page checksum is valid");

    page.back() ^= std::byte{1};
    const auto corrupted = nessodb::storage::verify_page_checksum(page);
    expect(!corrupted &&
               corrupted.error() == nessodb::storage::PageChecksumError::checksum_mismatch,
           "changed page fails checksum verification");

    expect(nessodb::storage::update_page_checksum(page).has_value(),
           "changed page can be sealed again");
    expect(nessodb::storage::verify_page_checksum(page).has_value(),
           "resealed page checksum is valid");
}

void test_database_header() {
    nessodb::storage::PageBuffer page;
    page.fill(std::byte{7});
    const nessodb::storage::DatabaseHeader expected{
        3,
        nessodb::common::PageId{2},
        nessodb::common::PageId{1},
    };

    const auto encoded = nessodb::storage::encode_database_header(page, expected);
    expect(encoded.has_value(), "database header is encoded");
    const auto decoded = nessodb::storage::decode_database_header(page);
    expect(decoded.has_value(), "database header is decoded");
    if (decoded) {
        expect(*decoded == expected, "database header round-trips exactly");
    }
    expect(page.back() == std::byte{0}, "database header clears reserved page bytes");

    nessodb::storage::PageBuffer initial_page{};
    const nessodb::storage::DatabaseHeader initial;
    expect(nessodb::storage::encode_database_header(initial_page, initial).has_value(),
           "initial database header is encoded without optional roots");
    expect(nessodb::storage::decode_database_header(initial_page) == initial,
           "initial database header round-trips");
}

void test_database_header_validation() {
    nessodb::storage::PageBuffer page{};
    const nessodb::storage::DatabaseHeader invalid_reference{
        2,
        nessodb::common::PageId{2},
        std::nullopt,
    };
    const auto invalid = nessodb::storage::encode_database_header(page, invalid_reference);
    expect(!invalid &&
               invalid.error() == nessodb::storage::DatabaseHeaderError::invalid_page_reference,
           "database header rejects reference outside page count");

    const auto encoded =
        nessodb::storage::encode_database_header(page, nessodb::storage::DatabaseHeader{});
    expect(encoded.has_value(), "database header validation page is encoded");
    expect(nessodb::storage::write_u32(page, 32, 8192).has_value(),
           "stored page size is changed for validation test");
    const auto wrong_page_size = nessodb::storage::decode_database_header(page);
    expect(!wrong_page_size && wrong_page_size.error() ==
                                   nessodb::storage::DatabaseHeaderError::unexpected_page_size,
           "database header rejects unexpected page size");

    std::span<std::byte> short_page{page.data(), page.size() - 1};
    const auto invalid_size =
        nessodb::storage::encode_database_header(short_page, nessodb::storage::DatabaseHeader{});
    expect(!invalid_size &&
               invalid_size.error() == nessodb::storage::DatabaseHeaderError::invalid_page_size,
           "database header requires one complete page");
}

}  // namespace

int main() {
    test_page_identifier();
    test_page_buffer();
    test_integer_codec();
    test_codec_bounds();
    test_page_header_round_trip();
    test_page_header_validation();
    test_empty_slotted_page();
    test_slotted_page_links();
    test_slotted_page_validation();
    test_slotted_page_records();
    test_slotted_page_deletion();
    test_slotted_page_update();
    test_slotted_page_capacity();
    test_crc32c();
    test_page_checksum();
    test_database_header();
    test_database_header_validation();

    if (failures != 0) {
        std::cerr << failures << " page assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
