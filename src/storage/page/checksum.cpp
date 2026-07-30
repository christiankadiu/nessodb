#include "storage/page/checksum.hpp"

#include "storage/page/codec.hpp"
#include "storage/page/page.hpp"
#include "storage/page/page_header.hpp"

namespace minidb::storage {
namespace {

inline constexpr std::uint32_t crc32c_polynomial = 0x82f63b78;
inline constexpr std::size_t checksum_size = sizeof(std::uint32_t);

std::uint32_t extend_crc32c(std::uint32_t checksum,
                            std::span<const std::byte> bytes) noexcept {
    for (const std::byte byte : bytes) {
        checksum ^= std::to_integer<std::uint8_t>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (checksum & 1U);
            checksum = (checksum >> 1U) ^ (crc32c_polynomial & mask);
        }
    }
    return checksum;
}

std::uint32_t page_crc32c(std::span<const std::byte> page) noexcept {
    std::uint32_t checksum = 0xffffffff;
    checksum = extend_crc32c(checksum, page.first(page_checksum_offset));

    constexpr std::byte zeros[checksum_size]{};
    checksum = extend_crc32c(checksum, zeros);
    checksum = extend_crc32c(
        checksum, page.subspan(page_checksum_offset + checksum_size));
    return checksum ^ 0xffffffff;
}

}  // namespace

std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept {
    return extend_crc32c(0xffffffff, bytes) ^ 0xffffffff;
}

std::expected<void, PageChecksumError> update_page_checksum(
    std::span<std::byte> page) noexcept {
    if (page.size() != page_size) {
        return std::unexpected(PageChecksumError::invalid_page_size);
    }
    if (!decode_page_header(page)) {
        return std::unexpected(PageChecksumError::invalid_page_header);
    }

    if (!write_u32(page, page_checksum_offset, page_crc32c(page))) {
        return std::unexpected(PageChecksumError::invalid_page_size);
    }
    return {};
}

std::expected<void, PageChecksumError> verify_page_checksum(
    std::span<const std::byte> page) noexcept {
    if (page.size() != page_size) {
        return std::unexpected(PageChecksumError::invalid_page_size);
    }
    if (!decode_page_header(page)) {
        return std::unexpected(PageChecksumError::invalid_page_header);
    }

    if (*read_u32(page, page_checksum_offset) != page_crc32c(page)) {
        return std::unexpected(PageChecksumError::checksum_mismatch);
    }
    return {};
}

}  // namespace minidb::storage
