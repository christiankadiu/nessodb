#include "index/key_codec.hpp"

#include <bit>
#include <cstdint>
#include <string>
#include <variant>

namespace minidb::index {
namespace {

constexpr std::byte integer_tag{0x10};
constexpr std::byte text_tag{0x20};
constexpr std::byte null_tag{0xff};
constexpr std::byte text_escape{0x00};
constexpr std::byte escaped_zero{0xff};
constexpr std::byte text_end{0x00};
constexpr std::uint64_t sign_bit = std::uint64_t{1} << 63;

void append_integer(EncodedKey& key, std::int64_t value) {
    std::uint64_t encoded = std::bit_cast<std::uint64_t>(value) ^ sign_bit;
    for (std::size_t index = 0; index < sizeof(encoded); ++index) {
        const auto shift = static_cast<unsigned>(
            (sizeof(encoded) - index - 1) * 8);
        key.push_back(static_cast<std::byte>((encoded >> shift) & 0xffU));
    }
}

void append_text(EncodedKey& key, const std::string& value) {
    for (const unsigned char character : value) {
        if (character == 0) {
            key.push_back(text_escape);
            key.push_back(escaped_zero);
        } else {
            key.push_back(static_cast<std::byte>(character));
        }
    }
    key.push_back(text_escape);
    key.push_back(text_end);
}

}  // namespace

EncodedKey encode_key(std::span<const types::Value> values) {
    EncodedKey key;
    key.push_back(static_cast<std::byte>(key_format_version));
    for (const auto& value : values) {
        if (std::holds_alternative<types::NullValue>(value)) {
            key.push_back(null_tag);
        } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
            key.push_back(integer_tag);
            append_integer(key, *integer);
        } else {
            key.push_back(text_tag);
            append_text(key, std::get<std::string>(value));
        }
    }
    return key;
}

std::expected<std::vector<types::Value>, KeyDecodeError> decode_key(
    std::span<const std::byte> key) {
    if (key.empty()) {
        return std::unexpected(KeyDecodeError::key_too_small);
    }
    if (std::to_integer<std::uint8_t>(key.front()) != key_format_version) {
        return std::unexpected(KeyDecodeError::unsupported_version);
    }

    std::vector<types::Value> values;
    std::size_t offset = 1;
    while (offset < key.size()) {
        const std::byte tag = key[offset++];
        if (tag == null_tag) {
            values.emplace_back(types::NullValue{});
            continue;
        }
        if (tag == integer_tag) {
            if (sizeof(std::uint64_t) > key.size() - offset) {
                return std::unexpected(KeyDecodeError::truncated_integer);
            }
            std::uint64_t encoded{};
            for (std::size_t index = 0; index < sizeof(encoded); ++index) {
                encoded = (encoded << 8) |
                          std::to_integer<std::uint8_t>(key[offset + index]);
            }
            offset += sizeof(encoded);
            values.emplace_back(
                std::bit_cast<std::int64_t>(encoded ^ sign_bit));
            continue;
        }
        if (tag != text_tag) {
            return std::unexpected(KeyDecodeError::unknown_type);
        }

        std::string text;
        bool terminated = false;
        while (offset < key.size()) {
            const std::byte character = key[offset++];
            if (character != text_escape) {
                text.push_back(
                    static_cast<char>(std::to_integer<unsigned char>(character)));
                continue;
            }
            if (offset == key.size()) {
                return std::unexpected(KeyDecodeError::unterminated_text);
            }
            const std::byte escaped = key[offset++];
            if (escaped == text_end) {
                terminated = true;
                break;
            }
            if (escaped != escaped_zero) {
                return std::unexpected(KeyDecodeError::invalid_text_escape);
            }
            text.push_back('\0');
        }
        if (!terminated) {
            return std::unexpected(KeyDecodeError::unterminated_text);
        }
        values.emplace_back(std::move(text));
    }
    return values;
}

std::strong_ordering compare_encoded_keys(
    std::span<const std::byte> left,
    std::span<const std::byte> right) noexcept {
    const std::size_t shared_size = left.size() < right.size()
                                        ? left.size()
                                        : right.size();
    for (std::size_t index = 0; index < shared_size; ++index) {
        const auto left_byte = std::to_integer<std::uint8_t>(left[index]);
        const auto right_byte = std::to_integer<std::uint8_t>(right[index]);
        if (left_byte < right_byte) {
            return std::strong_ordering::less;
        }
        if (left_byte > right_byte) {
            return std::strong_ordering::greater;
        }
    }
    return left.size() <=> right.size();
}

}  // namespace minidb::index
