#pragma once

#include "types/value.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace minidb::index {

inline constexpr std::uint8_t key_format_version = 1;

enum class KeyDecodeError {
    key_too_small,
    unsupported_version,
    unknown_type,
    truncated_integer,
    unterminated_text,
    invalid_text_escape,
};

using EncodedKey = std::vector<std::byte>;

[[nodiscard]] EncodedKey encode_key(
    std::span<const types::Value> values);
[[nodiscard]] std::expected<std::vector<types::Value>, KeyDecodeError>
decode_key(std::span<const std::byte> key);
[[nodiscard]] std::strong_ordering compare_encoded_keys(
    std::span<const std::byte> left,
    std::span<const std::byte> right) noexcept;

}  // namespace minidb::index
