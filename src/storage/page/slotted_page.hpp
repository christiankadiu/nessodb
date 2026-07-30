#pragma once

#include "common/page_id.hpp"
#include "storage/page/page_header.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace minidb::storage {

inline constexpr std::size_t heap_page_header_size = 40;
inline constexpr std::size_t slot_entry_size = 4;

enum class SlottedPageError {
    invalid_page_size,
    invalid_page_header,
    not_a_heap_page,
    corrupted_slot_directory,
};

class SlottedPage {
public:
    [[nodiscard]] static std::expected<SlottedPage, SlottedPageError> initialize(
        std::span<std::byte> page, common::PageId page_id) noexcept;
    [[nodiscard]] static std::expected<SlottedPage, SlottedPageError> open(
        std::span<std::byte> page) noexcept;

    [[nodiscard]] common::PageId page_id() const noexcept;
    [[nodiscard]] std::uint16_t slot_count() const noexcept;
    [[nodiscard]] std::size_t free_space() const noexcept;

private:
    SlottedPage(std::span<std::byte> page, PageHeader header) noexcept;

    std::span<std::byte> page_;
    PageHeader header_;
};

}  // namespace minidb::storage
