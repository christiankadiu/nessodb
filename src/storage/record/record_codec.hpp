#pragma once

#include "catalog/schema.hpp"
#include "storage/access/row.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

namespace minidb::storage {

inline constexpr std::uint16_t record_format_version = 1;
inline constexpr std::size_t record_header_size = 8;

enum class RecordEncodeError {
    column_count_mismatch,
    too_many_columns,
    type_mismatch,
    string_too_large,
    record_too_large,
};

[[nodiscard]] std::expected<std::vector<std::byte>, RecordEncodeError> encode_record(
    const Row& row, const catalog::TableSchema& schema);

}  // namespace minidb::storage
