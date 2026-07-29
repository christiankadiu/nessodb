#pragma once

#include "catalog/schema.hpp"

#include <cstdint>
#include <deque>
#include <expected>
#include <string_view>

namespace minidb::catalog {

enum class CatalogError {
    table_already_exists,
};

class Catalog {
public:
    [[nodiscard]] std::expected<void, CatalogError> create_table(TableSchema schema);
    [[nodiscard]] const TableSchema* find_table(std::string_view name) const noexcept;

private:
    std::deque<TableSchema> tables_;
    std::uint64_t next_table_id_{1};
};

}  // namespace minidb::catalog
