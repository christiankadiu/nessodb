#include "recovery/wal.hpp"

#include "common/page_id.hpp"
#include "common/transaction_id.hpp"
#include "storage/io/page_file.hpp"
#include "storage/page/checksum.hpp"
#include "storage/page/page.hpp"
#include "storage/page/page_header.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

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
        const auto suffix =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("nessodb-wal-test-" + std::to_string(suffix));
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

nessodb::storage::PageBuffer make_page(nessodb::common::PageId page_id,
                                      std::byte marker) {
    nessodb::storage::PageBuffer page{};
    const auto type = page_id.value == 0
        ? nessodb::storage::PageType::database_header
        : nessodb::storage::PageType::heap;
    const auto encoded = nessodb::storage::encode_page_header(
        page, nessodb::storage::PageHeader{type, page_id});
    expect(encoded.has_value(), "test page header is encoded");
    page[128] = marker;
    const auto checksummed = nessodb::storage::update_page_checksum(page);
    expect(checksummed.has_value(), "test page checksum is updated");
    return page;
}

void create_page_file(const std::filesystem::path& path,
                      const nessodb::storage::PageBuffer& first_page,
                      std::optional<nessodb::storage::PageBuffer> second_page =
                          std::nullopt) {
    auto file = nessodb::storage::PageFile::create(path);
    expect(file.has_value(), "page file is created");
    if (!file) {
        return;
    }
    expect(file->write_page(nessodb::common::PageId{0}, first_page).has_value(),
           "first page is written");
    if (second_page) {
        expect(file->write_page(nessodb::common::PageId{1}, *second_page)
                   .has_value(),
               "second page is written");
    }
    expect(file->flush().has_value(), "page file is flushed");
}

void test_committed_page_is_redone() {
    TemporaryDirectory directory;
    const auto database_path = directory.path() / "committed.mdb";
    create_page_file(database_path,
                     make_page(nessodb::common::PageId{0}, std::byte{0x10}));

    std::optional<nessodb::storage::PageBuffer> logged_page;
    {
        auto wal = nessodb::recovery::WriteAheadLog::create(database_path);
        expect(wal.has_value(), "WAL is created");
        if (!wal) {
            return;
        }
        expect(wal->begin(nessodb::common::TransactionId{1}, 1).has_value(),
               "transaction begin is logged");
        auto logged = wal->log_page_update(
            nessodb::common::PageId{1}, std::nullopt,
            make_page(nessodb::common::PageId{1}, std::byte{0x20}));
        expect(logged.has_value(), "new page image is logged");
        if (!logged) {
            return;
        }
        logged_page = logged->page;
        expect(wal->commit(nessodb::common::TransactionId{1}).has_value(),
               "transaction commit is logged");
    }

    {
        std::ofstream tail(nessodb::recovery::wal_path_for(database_path),
                           std::ios::binary | std::ios::app);
        const char partial_record[] = {'W', 'A', 'L'};
        tail.write(partial_record,
                   static_cast<std::streamsize>(sizeof(partial_record)));
    }

    auto file = nessodb::storage::PageFile::open(database_path);
    auto wal = nessodb::recovery::WriteAheadLog::open(database_path);
    expect(file.has_value() && wal.has_value(),
           "database and WAL reopen for recovery");
    if (!file || !wal || !logged_page) {
        return;
    }
    auto recovered = wal->recover(*file);
    expect(recovered.has_value(), "committed WAL is recovered");
    if (!recovered) {
        return;
    }
    expect(recovered->records_scanned == 3,
           "begin, update, and commit records are scanned");
    expect(recovered->pages_redone == 1 && recovered->pages_undone == 0,
           "committed page is redone without undo");
    expect(recovered->truncated_tail,
           "partial trailing WAL record is discarded");
    const auto page = file->read_page(nessodb::common::PageId{1});
    expect(page && *page == *logged_page,
           "recovery writes the durable committed page image");

    const auto second_recovery = wal->recover(*file);
    expect(second_recovery && second_recovery->records_scanned == 0,
           "successful recovery checkpoints the WAL");
}

void test_interrupted_transaction_is_undone() {
    TemporaryDirectory directory;
    const auto database_path = directory.path() / "interrupted.mdb";
    const auto header_page =
        make_page(nessodb::common::PageId{0}, std::byte{0x30});
    const auto before =
        make_page(nessodb::common::PageId{1}, std::byte{0x31});
    create_page_file(database_path, header_page, before);

    {
        auto file = nessodb::storage::PageFile::open(database_path);
        auto wal = nessodb::recovery::WriteAheadLog::create(database_path);
        expect(file.has_value() && wal.has_value(),
               "interrupted transaction fixtures open");
        if (!file || !wal) {
            return;
        }
        expect(wal->begin(nessodb::common::TransactionId{2}, 2).has_value(),
               "interrupted transaction begins");
        auto updated = wal->log_page_update(
            nessodb::common::PageId{1}, before,
            make_page(nessodb::common::PageId{1}, std::byte{0x32}));
        auto allocated = wal->log_page_update(
            nessodb::common::PageId{2}, std::nullopt,
            make_page(nessodb::common::PageId{2}, std::byte{0x33}));
        expect(updated.has_value() && allocated.has_value(),
               "interrupted page changes are logged");
        if (!updated || !allocated) {
            return;
        }
        expect(file->write_page(nessodb::common::PageId{1}, updated->page)
                   .has_value() &&
                   file->write_page(nessodb::common::PageId{2}, allocated->page)
                       .has_value() &&
                   file->flush().has_value(),
               "uncommitted pages reach the data file");
    }

    auto file = nessodb::storage::PageFile::open(database_path);
    auto wal = nessodb::recovery::WriteAheadLog::open(database_path);
    expect(file.has_value() && wal.has_value(),
           "interrupted transaction reopens");
    if (!file || !wal) {
        return;
    }
    auto recovered = wal->recover(*file);
    expect(recovered.has_value(), "interrupted transaction is recovered");
    if (!recovered) {
        return;
    }
    expect(recovered->pages_redone == 0 && recovered->pages_undone == 1,
           "existing uncommitted page is undone");
    const auto page_count = file->page_count();
    expect(page_count && *page_count == 2,
           "pages allocated by interrupted transaction are removed");
    const auto restored = file->read_page(nessodb::common::PageId{1});
    expect(restored && *restored == before,
           "before image restores the modified page");
}

void test_aborted_transaction_is_not_redone() {
    TemporaryDirectory directory;
    const auto database_path = directory.path() / "aborted.mdb";
    const auto header_page =
        make_page(nessodb::common::PageId{0}, std::byte{0x40});
    const auto before =
        make_page(nessodb::common::PageId{1}, std::byte{0x41});
    create_page_file(database_path, header_page, before);

    {
        auto wal = nessodb::recovery::WriteAheadLog::create(database_path);
        expect(wal.has_value(), "abort WAL is created");
        if (!wal) {
            return;
        }
        expect(wal->begin(nessodb::common::TransactionId{3}, 2).has_value(),
               "aborted transaction begins");
        expect(wal->log_page_update(
                       nessodb::common::PageId{1}, before,
                       make_page(nessodb::common::PageId{1}, std::byte{0x42}))
                   .has_value(),
               "aborted update is logged");
        expect(wal->abort(nessodb::common::TransactionId{3}).has_value(),
               "abort record is durable");
    }

    auto file = nessodb::storage::PageFile::open(database_path);
    auto wal = nessodb::recovery::WriteAheadLog::open(database_path);
    expect(file.has_value() && wal.has_value(),
           "aborted transaction reopens");
    if (!file || !wal) {
        return;
    }
    auto recovered = wal->recover(*file);
    expect(recovered && recovered->pages_redone == 0 &&
               recovered->pages_undone == 0,
           "completed abort is neither redone nor undone again");
    const auto unchanged = file->read_page(nessodb::common::PageId{1});
    expect(unchanged && *unchanged == before,
           "aborted page image does not replace stored data");
}

}  // namespace

int main() {
    test_committed_page_is_redone();
    test_interrupted_transaction_is_undone();
    test_aborted_transaction_is_not_redone();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
