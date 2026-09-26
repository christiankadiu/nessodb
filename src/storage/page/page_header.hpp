#pragma once

#include "common/page_id.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace nessodb::storage {

inline constexpr std::uint32_t page_magic = 0x5042444d;
inline constexpr std::uint16_t page_format_version = 1;
inline constexpr std::size_t page_header_size = 32;
inline constexpr std::size_t page_checksum_offset = 24;

enum class PageType : std::uint16_t {
    database_header = 1,
    heap = 2,
    index_leaf = 3,
    index_internal = 4,
};

struct PageHeader {
    PageType type;
    common::PageId page_id;
    std::uint64_t log_sequence_number{};
    std::uint32_t checksum{};

    friend bool operator==(const PageHeader&, const PageHeader&) = default;
};

enum class PageHeaderError {
    page_too_small,
    invalid_magic,
    unsupported_version,
    unknown_page_type,
    invalid_page_id,
    reserved_bytes_nonzero,
};

[[nodiscard]] std::expected<void, PageHeaderError> encode_page_header(
    std::span<std::byte> destination, const PageHeader& header) noexcept;
[[nodiscard]] std::expected<PageHeader, PageHeaderError> decode_page_header(
    std::span<const std::byte> source) noexcept;

}  // namespace nessodb::storage
