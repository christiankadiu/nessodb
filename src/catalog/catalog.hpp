#pragma once

#include "catalog/schema.hpp"
#include "common/table_id.hpp"

#include <cstdint>
#include <deque>
#include <expected>
#include <string_view>

namespace nessodb::catalog {

enum class CatalogError {
    table_already_exists,
    table_id_already_exists,
    invalid_table_id,
    table_id_exhausted,
};

class Catalog {
public:
    [[nodiscard]] std::expected<const TableSchema*, CatalogError> create_table(
        TableSchema schema);
    [[nodiscard]] std::expected<const TableSchema*, CatalogError> restore_table(
        TableSchema schema);
    [[nodiscard]] const TableSchema* find_table(std::string_view name) const noexcept;
    [[nodiscard]] const TableSchema* find_table(common::TableId table_id) const noexcept;

private:
    std::deque<TableSchema> tables_;
    std::uint64_t next_table_id_{1};
};

}  // namespace nessodb::catalog
