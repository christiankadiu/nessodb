#include "index/key_codec.hpp"

#include "types/value.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using nessodb::index::EncodedKey;
using nessodb::index::KeyDecodeError;
using nessodb::types::NullValue;
using nessodb::types::Value;

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

EncodedKey encode(std::initializer_list<Value> values) {
    const std::vector<Value> owned{values};
    return nessodb::index::encode_key(owned);
}

bool less(const EncodedKey& left, const EncodedKey& right) {
    return nessodb::index::compare_encoded_keys(left, right) ==
           std::strong_ordering::less;
}

void test_round_trip() {
    const std::vector<Value> values{
        NullValue{},
        std::numeric_limits<std::int64_t>::min(),
        std::int64_t{-1},
        std::int64_t{0},
        std::numeric_limits<std::int64_t>::max(),
        std::string{"A\0B", 3},
        std::string{"\xff", 1},
    };

    const auto encoded = nessodb::index::encode_key(values);
    const auto decoded = nessodb::index::decode_key(encoded);
    expect(decoded && *decoded == values,
           "all supported values round-trip through a composite key");

    const std::vector<Value> empty_values;
    const auto empty_key = nessodb::index::encode_key(empty_values);
    const auto decoded_empty = nessodb::index::decode_key(empty_key);
    expect(decoded_empty && decoded_empty->empty(),
           "an empty composite key round-trips");
}

void test_integer_order() {
    const std::vector<std::int64_t> values{
        std::numeric_limits<std::int64_t>::min(),
        -100,
        -1,
        0,
        1,
        100,
        std::numeric_limits<std::int64_t>::max(),
    };
    for (std::size_t index = 1; index < values.size(); ++index) {
        expect(less(encode({values[index - 1]}), encode({values[index]})),
               "encoded integer bytes preserve signed numeric order");
    }
}

void test_text_order() {
    const std::vector<std::string> values{
        "",
        std::string{"\0", 1},
        std::string{"\0a", 2},
        "a",
        std::string{"a\0", 2},
        "aa",
        "b",
        std::string{"\xff", 1},
    };
    for (std::size_t index = 1; index < values.size(); ++index) {
        expect(values[index - 1] < values[index],
               "text fixture follows native byte order");
        expect(less(encode({values[index - 1]}), encode({values[index]})),
               "encoded text bytes preserve prefix and binary order");
    }
}

void test_composite_order() {
    const auto first = encode({std::int64_t{1}, std::string{"a"}});
    const auto second = encode({std::int64_t{1}, std::string{"b"}});
    const auto third = encode({std::int64_t{2}, std::string{}});
    const auto null = encode({NullValue{}});

    expect(less(first, second) && less(second, third),
           "composite keys compare one component at a time");
    expect(less(encode({std::int64_t{1}}), first),
           "a composite prefix sorts before its extension");
    expect(less(third, null), "NULL sorts after non-null values");
    expect(nessodb::index::compare_encoded_keys(first, first) ==
               std::strong_ordering::equal,
           "identical encoded keys compare equal");
}

void test_decode_validation() {
    const auto expect_error = [](EncodedKey key, KeyDecodeError error,
                                 std::string_view description) {
        const auto decoded = nessodb::index::decode_key(key);
        expect(!decoded && decoded.error() == error, description);
    };

    expect_error({}, KeyDecodeError::key_too_small,
                 "missing version is rejected");
    expect_error({std::byte{2}}, KeyDecodeError::unsupported_version,
                 "unknown key format version is rejected");
    expect_error({std::byte{1}, std::byte{0x7f}},
                 KeyDecodeError::unknown_type,
                 "unknown value tag is rejected");
    expect_error({std::byte{1}, std::byte{0x10}, std::byte{0}},
                 KeyDecodeError::truncated_integer,
                 "truncated integer is rejected");
    expect_error({std::byte{1}, std::byte{0x20}, std::byte{'a'}},
                 KeyDecodeError::unterminated_text,
                 "unterminated text is rejected");
    expect_error({std::byte{1}, std::byte{0x20}, std::byte{0},
                  std::byte{1}},
                 KeyDecodeError::invalid_text_escape,
                 "invalid text escape is rejected");
}

}  // namespace

int main() {
    test_round_trip();
    test_integer_order();
    test_text_order();
    test_composite_order();
    test_decode_validation();

    if (failures != 0) {
        std::cerr << failures << " key codec assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
