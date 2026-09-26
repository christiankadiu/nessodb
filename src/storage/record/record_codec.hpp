#pragma once

#include "catalog/schema.hpp"
#include "storage/access/row.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace nessodb::storage {

inline constexpr std::uint16_t record_format_version = 1;
inline constexpr std::size_t record_header_size = 8;

enum class RecordEncodeError {
    column_count_mismatch,
    too_many_columns,
    type_mismatch,
    string_too_large,
    record_too_large,
};

enum class RecordDecodeError {
    record_too_small,
    unsupported_version,
    schema_too_wide,
    column_count_mismatch,
    size_mismatch,
    reserved_null_bits_nonzero,
    truncated_value,
    payload_size_mismatch,
};

[[nodiscard]] std::expected<std::vector<std::byte>, RecordEncodeError> encode_record(
    const Row& row, const catalog::TableSchema& schema);
[[nodiscard]] std::expected<Row, RecordDecodeError> decode_record(
    std::span<const std::byte> record, const catalog::TableSchema& schema);

}  // namespace nessodb::storage
