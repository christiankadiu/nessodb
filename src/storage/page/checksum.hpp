#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace nessodb::storage {

enum class PageChecksumError {
    invalid_page_size,
    invalid_page_header,
    checksum_mismatch,
};

[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] std::expected<void, PageChecksumError> update_page_checksum(
    std::span<std::byte> page) noexcept;
[[nodiscard]] std::expected<void, PageChecksumError> verify_page_checksum(
    std::span<const std::byte> page) noexcept;

}  // namespace nessodb::storage
