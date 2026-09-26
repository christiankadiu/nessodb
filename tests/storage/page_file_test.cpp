#include "storage/io/page_file.hpp"

#include "common/page_id.hpp"
#include "storage/page/page.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("nessodb-page-file-test-" + std::to_string(suffix));
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

void test_create_and_open() {
    TemporaryDirectory directory;
    const auto database_path = directory.path() / "database.mdb";

    {
        auto created = nessodb::storage::PageFile::create(database_path);
        expect(created.has_value(), "new page file is created");
        if (created) {
            expect(created->is_open(), "created page file remains open");
            expect(created->path() == database_path, "created page file preserves its path");
        }

        const auto duplicate = nessodb::storage::PageFile::create(database_path);
        expect(!duplicate &&
                   duplicate.error() == nessodb::storage::PageFileError::path_already_exists,
               "existing page file is never overwritten");
    }

    auto opened = nessodb::storage::PageFile::open(database_path);
    expect(opened.has_value(), "existing page file is opened");
    if (opened) {
        expect(opened->is_open(), "opened page file remains open");
        expect(opened->path() == database_path, "opened page file preserves its path");
    }
}

void test_invalid_paths() {
    TemporaryDirectory directory;
    const auto missing_path = directory.path() / "missing.mdb";

    const auto empty_create = nessodb::storage::PageFile::create({});
    expect(!empty_create && empty_create.error() == nessodb::storage::PageFileError::empty_path,
           "empty creation path is rejected");

    const auto missing = nessodb::storage::PageFile::open(missing_path);
    expect(!missing && missing.error() == nessodb::storage::PageFileError::path_not_found,
           "missing page file is rejected");

    const auto directory_open = nessodb::storage::PageFile::open(directory.path());
    expect(!directory_open &&
               directory_open.error() == nessodb::storage::PageFileError::not_regular_file,
           "directory cannot be opened as a page file");
}

void test_page_io() {
    TemporaryDirectory directory;
    const auto database_path = directory.path() / "pages.mdb";
    auto file = nessodb::storage::PageFile::create(database_path);
    expect(file.has_value(), "page I/O test file is created");
    if (!file) {
        return;
    }

    nessodb::storage::PageBuffer first_page{};
    first_page.front() = std::byte{0x12};
    first_page.back() = std::byte{0x34};
    expect(file->write_page(nessodb::common::PageId{0}, first_page).has_value(),
           "first page is appended");

    const auto read = file->read_page(nessodb::common::PageId{0});
    expect(read && *read == first_page, "written page round-trips exactly");

    nessodb::storage::PageBuffer replacement{};
    replacement[17] = std::byte{0x56};
    expect(file->write_page(nessodb::common::PageId{0}, replacement).has_value(),
           "existing page is overwritten");
    const auto reread = file->read_page(nessodb::common::PageId{0});
    expect(reread && *reread == replacement, "overwritten page contains new bytes");

    const auto missing = file->read_page(nessodb::common::PageId{1});
    expect(!missing && missing.error() == nessodb::storage::PageFileError::page_not_found,
           "page beyond end of file is rejected");

    const auto gap = file->write_page(nessodb::common::PageId{2}, first_page);
    expect(!gap && gap.error() == nessodb::storage::PageFileError::non_contiguous_write,
           "page write cannot leave a gap");

    const auto invalid = file->read_page(nessodb::common::PageId{});
    expect(!invalid && invalid.error() == nessodb::storage::PageFileError::invalid_page_id,
           "invalid page identifier is rejected");
}

}  // namespace

int main() {
    test_create_and_open();
    test_invalid_paths();
    test_page_io();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
