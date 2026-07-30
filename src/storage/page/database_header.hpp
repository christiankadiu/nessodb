#pragma once

#include "common/page_id.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace minidb::storage {

inline constexpr std::size_t database_header_size = 64;

struct DatabaseHeader {
    std::uint64_t page_count{1};
    std::optional<common::PageId> catalog_root;
    std::optional<common::PageId> free_list_head;

    friend bool operator==(const DatabaseHeader&, const DatabaseHeader&) = default;
};

enum class DatabaseHeaderError {
    invalid_page_size,
    invalid_page_header,
    unexpected_page_size,
    invalid_page_count,
    invalid_page_reference,
    reserved_bytes_nonzero,
};

[[nodiscard]] std::expected<void, DatabaseHeaderError> encode_database_header(
    std::span<std::byte> destination, const DatabaseHeader& header) noexcept;
[[nodiscard]] std::expected<DatabaseHeader, DatabaseHeaderError> decode_database_header(
    std::span<const std::byte> source) noexcept;

}  // namespace minidb::storage
