#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace minidb::storage {

enum class CodecError {
    out_of_bounds,
};

namespace detail {

template <std::unsigned_integral Integer>
[[nodiscard]] constexpr std::expected<Integer, CodecError> read_little_endian(
    std::span<const std::byte> source, std::size_t offset) noexcept {
    if (offset > source.size() || sizeof(Integer) > source.size() - offset) {
        return std::unexpected(CodecError::out_of_bounds);
    }

    Integer value{};
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        const auto byte = std::to_integer<std::uint8_t>(source[offset + index]);
        value |= static_cast<Integer>(static_cast<Integer>(byte) << (index * 8));
    }
    return value;
}

template <std::unsigned_integral Integer>
[[nodiscard]] constexpr std::expected<void, CodecError> write_little_endian(
    std::span<std::byte> destination, std::size_t offset, Integer value) noexcept {
    if (offset > destination.size() || sizeof(Integer) > destination.size() - offset) {
        return std::unexpected(CodecError::out_of_bounds);
    }

    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        destination[offset + index] =
            static_cast<std::byte>((value >> (index * 8)) & static_cast<Integer>(0xff));
    }
    return {};
}

}  // namespace detail

[[nodiscard]] constexpr std::expected<std::uint16_t, CodecError> read_u16(
    std::span<const std::byte> source, std::size_t offset) noexcept {
    return detail::read_little_endian<std::uint16_t>(source, offset);
}

[[nodiscard]] constexpr std::expected<std::uint32_t, CodecError> read_u32(
    std::span<const std::byte> source, std::size_t offset) noexcept {
    return detail::read_little_endian<std::uint32_t>(source, offset);
}

[[nodiscard]] constexpr std::expected<std::uint64_t, CodecError> read_u64(
    std::span<const std::byte> source, std::size_t offset) noexcept {
    return detail::read_little_endian<std::uint64_t>(source, offset);
}

[[nodiscard]] constexpr std::expected<void, CodecError> write_u16(
    std::span<std::byte> destination, std::size_t offset, std::uint16_t value) noexcept {
    return detail::write_little_endian(destination, offset, value);
}

[[nodiscard]] constexpr std::expected<void, CodecError> write_u32(
    std::span<std::byte> destination, std::size_t offset, std::uint32_t value) noexcept {
    return detail::write_little_endian(destination, offset, value);
}

[[nodiscard]] constexpr std::expected<void, CodecError> write_u64(
    std::span<std::byte> destination, std::size_t offset, std::uint64_t value) noexcept {
    return detail::write_little_endian(destination, offset, value);
}

}  // namespace minidb::storage
