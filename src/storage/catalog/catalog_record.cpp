#include "storage/catalog/catalog_record.hpp"

#include "types/logical_type.hpp"
#include "types/value.hpp"

#include <cstddef>
#include <limits>
#include <utility>

namespace minidb::storage {
namespace {

inline constexpr std::int64_t table_entry_kind = 1;
inline constexpr std::int64_t column_entry_kind = 2;
inline constexpr std::int64_t integer_type_code = 1;
inline constexpr std::int64_t text_type_code = 2;
inline constexpr std::size_t catalog_field_count = 7;

std::expected<std::int64_t, CatalogRecordError> encode_identifier(
    std::uint64_t value, CatalogRecordError invalid_error) noexcept {
    if (value == 0) {
        return std::unexpected(invalid_error);
    }
    if (value > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
        return std::unexpected(CatalogRecordError::numeric_value_out_of_range);
    }
    return static_cast<std::int64_t>(value);
}

std::expected<std::int64_t, CatalogRecordError> encode_logical_type(
    types::LogicalType type) noexcept {
    switch (type) {
        case types::LogicalType::integer:
            return integer_type_code;
        case types::LogicalType::text:
            return text_type_code;
    }
    return std::unexpected(CatalogRecordError::invalid_logical_type);
}

std::expected<types::LogicalType, CatalogRecordError> decode_logical_type(
    std::int64_t code) noexcept {
    switch (code) {
        case integer_type_code:
            return types::LogicalType::integer;
        case text_type_code:
            return types::LogicalType::text;
        default:
            return std::unexpected(CatalogRecordError::invalid_logical_type);
    }
}

const std::int64_t* integer_field(const Row& row, std::size_t index) noexcept {
    return std::get_if<std::int64_t>(&row.values[index]);
}

const std::string* text_field(const Row& row, std::size_t index) noexcept {
    return std::get_if<std::string>(&row.values[index]);
}

bool null_field(const Row& row, std::size_t index) noexcept {
    return std::holds_alternative<types::NullValue>(row.values[index]);
}

}  // namespace

const catalog::TableSchema& catalog_record_schema() {
    static const catalog::TableSchema schema{
        "minidb_catalog",
        {
            {"format_version", types::LogicalType::integer},
            {"entry_kind", types::LogicalType::integer},
            {"table_id", types::LogicalType::integer},
            {"ordinal", types::LogicalType::integer},
            {"name", types::LogicalType::text},
            {"logical_type", types::LogicalType::integer},
            {"first_page_id", types::LogicalType::integer},
        },
    };
    return schema;
}

std::expected<Row, CatalogRecordError> encode_catalog_record(
    const CatalogRecord& record) {
    if (const auto* table = std::get_if<CatalogTableRecord>(&record)) {
        auto table_id = encode_identifier(
            table->table_id.value, CatalogRecordError::invalid_table_id);
        auto first_page_id = encode_identifier(
            table->first_page_id.value,
            CatalogRecordError::invalid_first_page_id);
        if (!table_id) {
            return std::unexpected(table_id.error());
        }
        if (!first_page_id) {
            return std::unexpected(first_page_id.error());
        }
        if (table->name.empty()) {
            return std::unexpected(CatalogRecordError::empty_name);
        }
        return Row{{catalog_record_format_version, table_entry_kind, *table_id,
                    types::NullValue{}, table->name, types::NullValue{},
                    *first_page_id}};
    }

    const auto& column = std::get<CatalogColumnRecord>(record);
    auto table_id = encode_identifier(
        column.table_id.value, CatalogRecordError::invalid_table_id);
    if (!table_id) {
        return std::unexpected(table_id.error());
    }
    if (column.ordinal > static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max())) {
        return std::unexpected(CatalogRecordError::numeric_value_out_of_range);
    }
    if (column.column.name.empty()) {
        return std::unexpected(CatalogRecordError::empty_name);
    }
    auto logical_type = encode_logical_type(column.column.type);
    if (!logical_type) {
        return std::unexpected(logical_type.error());
    }
    return Row{{catalog_record_format_version, column_entry_kind, *table_id,
                static_cast<std::int64_t>(column.ordinal), column.column.name,
                *logical_type, types::NullValue{}}};
}

std::expected<CatalogRecord, CatalogRecordError> decode_catalog_record(
    const Row& row) {
    if (row.values.size() != catalog_field_count) {
        return std::unexpected(CatalogRecordError::invalid_field_count);
    }
    const auto* format_version = integer_field(row, 0);
    const auto* entry_kind = integer_field(row, 1);
    const auto* table_id = integer_field(row, 2);
    const auto* name = text_field(row, 4);
    if (format_version == nullptr || entry_kind == nullptr || table_id == nullptr ||
        name == nullptr) {
        return std::unexpected(CatalogRecordError::field_type_mismatch);
    }
    if (*format_version != catalog_record_format_version) {
        return std::unexpected(CatalogRecordError::unsupported_format_version);
    }
    if (*table_id <= 0) {
        return std::unexpected(CatalogRecordError::invalid_table_id);
    }
    if (name->empty()) {
        return std::unexpected(CatalogRecordError::empty_name);
    }

    const common::TableId decoded_table_id{static_cast<std::uint64_t>(*table_id)};
    if (*entry_kind == table_entry_kind) {
        const auto* first_page_id = integer_field(row, 6);
        if (!null_field(row, 3) || !null_field(row, 5) ||
            first_page_id == nullptr) {
            return std::unexpected(CatalogRecordError::field_type_mismatch);
        }
        if (*first_page_id <= 0) {
            return std::unexpected(CatalogRecordError::invalid_first_page_id);
        }
        return CatalogRecord{CatalogTableRecord{
            decoded_table_id, *name,
            common::PageId{static_cast<std::uint64_t>(*first_page_id)}}};
    }
    if (*entry_kind == column_entry_kind) {
        const auto* ordinal = integer_field(row, 3);
        const auto* logical_type = integer_field(row, 5);
        if (ordinal == nullptr || logical_type == nullptr || !null_field(row, 6)) {
            return std::unexpected(CatalogRecordError::field_type_mismatch);
        }
        if (*ordinal < 0) {
            return std::unexpected(CatalogRecordError::invalid_ordinal);
        }
        auto decoded_type = decode_logical_type(*logical_type);
        if (!decoded_type) {
            return std::unexpected(decoded_type.error());
        }
        return CatalogRecord{CatalogColumnRecord{
            decoded_table_id, static_cast<std::uint64_t>(*ordinal),
            catalog::ColumnSchema{*name, *decoded_type}}};
    }
    return std::unexpected(CatalogRecordError::unknown_entry_kind);
}

}  // namespace minidb::storage
