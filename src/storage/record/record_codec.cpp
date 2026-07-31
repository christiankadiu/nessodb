#include "storage/record/record_codec.hpp"

#include "storage/page/codec.hpp"
#include "types/logical_type.hpp"
#include "types/value.hpp"

#include <bit>
#include <cstring>
#include <limits>
#include <string>
#include <variant>

namespace minidb::storage {
namespace {

constexpr std::size_t null_bitmap_size(std::size_t column_count) noexcept {
    constexpr std::size_t bits_per_byte = 8;
    return (column_count + bits_per_byte - 1) / bits_per_byte;
}

bool value_matches_type(const types::Value& value, types::LogicalType type) noexcept {
    if (std::holds_alternative<types::NullValue>(value)) {
        return true;
    }
    switch (type) {
        case types::LogicalType::integer:
            return std::holds_alternative<std::int64_t>(value);
        case types::LogicalType::text:
            return std::holds_alternative<std::string>(value);
    }
    return false;
}

std::expected<std::uint32_t, RecordEncodeError> payload_size(
    const Row& row, const catalog::TableSchema& schema) noexcept {
    std::uint64_t size = 0;
    for (std::size_t index = 0; index < row.values.size(); ++index) {
        const auto& value = row.values[index];
        const auto type = schema.columns[index].type;
        if (!value_matches_type(value, type)) {
            return std::unexpected(RecordEncodeError::type_mismatch);
        }
        if (std::holds_alternative<types::NullValue>(value)) {
            continue;
        }

        if (type == types::LogicalType::integer) {
            size += sizeof(std::uint64_t);
        } else {
            const auto& text = std::get<std::string>(value);
            if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
                return std::unexpected(RecordEncodeError::string_too_large);
            }
            size += sizeof(std::uint32_t);
            size += static_cast<std::uint64_t>(text.size());
        }
        if (size > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(RecordEncodeError::record_too_large);
        }
    }
    return static_cast<std::uint32_t>(size);
}

}  // namespace

std::expected<std::vector<std::byte>, RecordEncodeError> encode_record(
    const Row& row, const catalog::TableSchema& schema) {
    if (row.values.size() != schema.columns.size()) {
        return std::unexpected(RecordEncodeError::column_count_mismatch);
    }
    if (row.values.size() > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(RecordEncodeError::too_many_columns);
    }

    const auto payload = payload_size(row, schema);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    const auto bitmap_size = null_bitmap_size(row.values.size());
    const auto metadata_size = record_header_size + bitmap_size;
    if (*payload > std::numeric_limits<std::size_t>::max() - metadata_size) {
        return std::unexpected(RecordEncodeError::record_too_large);
    }
    std::vector<std::byte> record(metadata_size + *payload);
    if (!write_u16(record, 0, static_cast<std::uint16_t>(row.values.size())) ||
        !write_u16(record, 2, record_format_version) || !write_u32(record, 4, *payload)) {
        return std::unexpected(RecordEncodeError::record_too_large);
    }

    std::size_t payload_offset = record_header_size + bitmap_size;
    for (std::size_t index = 0; index < row.values.size(); ++index) {
        const auto& value = row.values[index];
        if (std::holds_alternative<types::NullValue>(value)) {
            const auto bit = static_cast<std::uint8_t>(1U << (index % 8));
            record[record_header_size + index / 8] |= static_cast<std::byte>(bit);
            continue;
        }

        if (schema.columns[index].type == types::LogicalType::integer) {
            const auto integer = std::bit_cast<std::uint64_t>(std::get<std::int64_t>(value));
            if (!write_u64(record, payload_offset, integer)) {
                return std::unexpected(RecordEncodeError::record_too_large);
            }
            payload_offset += sizeof(std::uint64_t);
        } else {
            const auto& text = std::get<std::string>(value);
            if (!write_u32(record, payload_offset, static_cast<std::uint32_t>(text.size()))) {
                return std::unexpected(RecordEncodeError::record_too_large);
            }
            payload_offset += sizeof(std::uint32_t);
            std::memcpy(record.data() + payload_offset, text.data(), text.size());
            payload_offset += text.size();
        }
    }
    return record;
}

}  // namespace minidb::storage
