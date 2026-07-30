#pragma once

#include "common/page_id.hpp"
#include "storage/page/page_header.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>

namespace minidb::storage {

inline constexpr std::size_t heap_page_header_size = 40;
inline constexpr std::size_t slot_entry_size = 4;

struct SlotId {
    static constexpr std::uint16_t invalid_value =
        std::numeric_limits<std::uint16_t>::max();

    std::uint16_t value{invalid_value};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return value != invalid_value;
    }

    friend bool operator==(const SlotId&, const SlotId&) = default;
};

enum class SlottedPageError {
    invalid_page_size,
    invalid_page_header,
    not_a_heap_page,
    corrupted_slot_directory,
    empty_record,
    page_full,
    invalid_slot,
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
    [[nodiscard]] std::expected<SlotId, SlottedPageError> insert(
        std::span<const std::byte> record) noexcept;
    [[nodiscard]] std::expected<std::span<const std::byte>, SlottedPageError> read(
        SlotId slot_id) const noexcept;

private:
    SlottedPage(std::span<std::byte> page, PageHeader header) noexcept;

    std::span<std::byte> page_;
    PageHeader header_;
};

}  // namespace minidb::storage
