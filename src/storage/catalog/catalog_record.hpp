#pragma once

#include "catalog/schema.hpp"
#include "common/page_id.hpp"
#include "common/table_id.hpp"
#include "storage/access/row.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <variant>

namespace nessodb::storage {

inline constexpr std::int64_t catalog_record_format_version = 2;

struct CatalogTableRecord {
    common::TableId table_id;
    std::string name;
    common::PageId first_page_id;
    std::uint64_t column_count;
};

struct CatalogColumnRecord {
    common::TableId table_id;
    std::uint64_t ordinal;
    catalog::ColumnSchema column;
};

struct CatalogIndexRecord {
    common::TableId table_id;
    std::uint64_t ordinal;
    std::string name;
    common::PageId root_page_id;
    bool unique{};
    bool primary_key{};
};

using CatalogRecord =
    std::variant<CatalogTableRecord, CatalogColumnRecord,
                 CatalogIndexRecord>;

enum class CatalogRecordError {
    invalid_field_count,
    field_type_mismatch,
    unsupported_format_version,
    unknown_entry_kind,
    invalid_table_id,
    invalid_ordinal,
    invalid_column_count,
    empty_name,
    invalid_logical_type,
    invalid_first_page_id,
    invalid_index_root,
    invalid_index_flags,
    numeric_value_out_of_range,
};

[[nodiscard]] const catalog::TableSchema& catalog_record_schema();
[[nodiscard]] std::expected<Row, CatalogRecordError> encode_catalog_record(
    const CatalogRecord& record);
[[nodiscard]] std::expected<CatalogRecord, CatalogRecordError> decode_catalog_record(
    const Row& row);

}  // namespace nessodb::storage
