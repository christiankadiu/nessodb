#include "storage/record/record_codec.hpp"

#include "catalog/schema.hpp"
#include "storage/access/row.hpp"
#include "storage/page/codec.hpp"
#include "types/logical_type.hpp"
#include "types/value.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

void test_encode_record() {
    const nessodb::catalog::TableSchema schema{
        "items",
        {
            {"quantity", nessodb::types::LogicalType::integer},
            {"name", nessodb::types::LogicalType::text},
            {"note", nessodb::types::LogicalType::text},
        },
    };
    const nessodb::storage::Row row{{std::int64_t{-2}, std::string{"Hi"},
                                    nessodb::types::NullValue{}}};

    const auto encoded = nessodb::storage::encode_record(row, schema);
    expect(encoded.has_value(), "valid row is encoded");
    if (!encoded) {
        return;
    }

    expect(encoded->size() == 23, "record has compact expected size");
    expect(nessodb::storage::read_u16(*encoded, 0) == 3,
           "record stores column count");
    expect(nessodb::storage::read_u16(*encoded, 2) ==
               nessodb::storage::record_format_version,
           "record stores format version");
    expect(nessodb::storage::read_u32(*encoded, 4) == 14,
           "record stores payload size");
    expect((*encoded)[8] == std::byte{0x04}, "record null bitmap marks third column");
    expect(nessodb::storage::read_u64(*encoded, 9) == 0xfffffffffffffffeULL,
           "negative integer preserves its bit pattern");
    expect(nessodb::storage::read_u32(*encoded, 17) == 2,
           "text value stores byte length");
    expect((*encoded)[21] == std::byte{'H'} && (*encoded)[22] == std::byte{'i'},
           "text bytes follow their length");
}

void test_encode_validation() {
    const nessodb::catalog::TableSchema schema{
        "items",
        {{"quantity", nessodb::types::LogicalType::integer}},
    };

    const auto wrong_count = nessodb::storage::encode_record(nessodb::storage::Row{}, schema);
    expect(!wrong_count &&
               wrong_count.error() == nessodb::storage::RecordEncodeError::column_count_mismatch,
           "row with wrong column count is rejected");

    const nessodb::storage::Row wrong_type{{std::string{"not an integer"}}};
    const auto mismatch = nessodb::storage::encode_record(wrong_type, schema);
    expect(!mismatch && mismatch.error() == nessodb::storage::RecordEncodeError::type_mismatch,
           "row value that disagrees with schema is rejected");

    const auto empty = nessodb::storage::encode_record(
        nessodb::storage::Row{}, nessodb::catalog::TableSchema{"empty", {}});
    expect(empty && empty->size() == nessodb::storage::record_header_size,
           "empty row has only the fixed record header");
}

void test_decode_record() {
    const nessodb::catalog::TableSchema schema{
        "items",
        {
            {"quantity", nessodb::types::LogicalType::integer},
            {"name", nessodb::types::LogicalType::text},
            {"note", nessodb::types::LogicalType::text},
        },
    };
    const nessodb::storage::Row row{{
        std::numeric_limits<std::int64_t>::min(),
        std::string{"A\0B", 3},
        nessodb::types::NullValue{},
    }};

    const auto encoded = nessodb::storage::encode_record(row, schema);
    expect(encoded.has_value(), "round-trip row is encoded");
    if (!encoded) {
        return;
    }
    const auto decoded = nessodb::storage::decode_record(*encoded, schema);
    expect(decoded.has_value(), "valid record is decoded");
    if (decoded) {
        expect(decoded->values == row.values,
               "integers, binary strings and nulls round-trip exactly");
    }

    const nessodb::catalog::TableSchema empty_schema{"empty", {}};
    const auto empty_record = nessodb::storage::encode_record(
        nessodb::storage::Row{}, empty_schema);
    expect(empty_record && nessodb::storage::decode_record(*empty_record, empty_schema),
           "empty record is decoded");
}

void test_decode_validation() {
    const nessodb::catalog::TableSchema schema{
        "items",
        {{"name", nessodb::types::LogicalType::text}},
    };
    const auto encoded = nessodb::storage::encode_record(
        nessodb::storage::Row{{std::string{"Hi"}}}, schema);
    if (!encoded) {
        expect(false, "validation record is encoded");
        return;
    }

    const auto short_record = nessodb::storage::decode_record(
        std::span<const std::byte>{encoded->data(), nessodb::storage::record_header_size - 1},
        schema);
    expect(!short_record &&
               short_record.error() == nessodb::storage::RecordDecodeError::record_too_small,
           "record shorter than fixed header is rejected");

    auto unknown_version = *encoded;
    expect(nessodb::storage::write_u16(unknown_version, 2, 2).has_value(),
           "record version is changed for validation");
    const auto unsupported = nessodb::storage::decode_record(unknown_version, schema);
    expect(!unsupported &&
               unsupported.error() == nessodb::storage::RecordDecodeError::unsupported_version,
           "unknown record format version is rejected");

    auto reserved_bitmap = *encoded;
    reserved_bitmap[8] = std::byte{0x80};
    const auto reserved = nessodb::storage::decode_record(reserved_bitmap, schema);
    expect(!reserved && reserved.error() ==
                            nessodb::storage::RecordDecodeError::reserved_null_bits_nonzero,
           "unused null bitmap bits must be zero");

    auto truncated = *encoded;
    truncated.pop_back();
    expect(nessodb::storage::write_u32(truncated, 4, 5).has_value(),
           "truncated payload size is adjusted for validation");
    const auto missing_text_byte = nessodb::storage::decode_record(truncated, schema);
    expect(!missing_text_byte &&
               missing_text_byte.error() == nessodb::storage::RecordDecodeError::truncated_value,
           "text that exceeds remaining payload is rejected");

    auto trailing_payload = *encoded;
    trailing_payload.push_back(std::byte{0});
    expect(nessodb::storage::write_u32(trailing_payload, 4, 7).has_value(),
           "payload size includes trailing byte for validation");
    const auto trailing = nessodb::storage::decode_record(trailing_payload, schema);
    expect(!trailing && trailing.error() ==
                            nessodb::storage::RecordDecodeError::payload_size_mismatch,
           "unconsumed payload bytes are rejected");
}

}  // namespace

int main() {
    test_encode_record();
    test_encode_validation();
    test_decode_record();
    test_decode_validation();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
