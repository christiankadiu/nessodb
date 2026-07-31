#include "catalog/catalog.hpp"

#include "common/identifier.hpp"

#include <limits>
#include <utility>

namespace minidb::catalog {

std::expected<const TableSchema*, CatalogError> Catalog::create_table(TableSchema schema) {
    if (find_table(schema.name) != nullptr) {
        return std::unexpected(CatalogError::table_already_exists);
    }
    if (next_table_id_ == 0) {
        return std::unexpected(CatalogError::table_id_exhausted);
    }

    schema.id = common::TableId{next_table_id_};
    ++next_table_id_;
    tables_.push_back(std::move(schema));
    return &tables_.back();
}

std::expected<const TableSchema*, CatalogError> Catalog::restore_table(
    TableSchema schema) {
    if (!schema.id.is_valid()) {
        return std::unexpected(CatalogError::invalid_table_id);
    }
    if (find_table(schema.name) != nullptr) {
        return std::unexpected(CatalogError::table_already_exists);
    }
    if (find_table(schema.id) != nullptr) {
        return std::unexpected(CatalogError::table_id_already_exists);
    }

    if (next_table_id_ != 0 && schema.id.value >= next_table_id_) {
        next_table_id_ =
            schema.id.value == std::numeric_limits<std::uint64_t>::max()
                ? 0
                : schema.id.value + 1;
    }
    tables_.push_back(std::move(schema));
    return &tables_.back();
}

const TableSchema* Catalog::find_table(std::string_view name) const noexcept {
    for (const auto& table : tables_) {
        if (common::identifiers_equal(table.name, name)) {
            return &table;
        }
    }
    return nullptr;
}

const TableSchema* Catalog::find_table(common::TableId table_id) const noexcept {
    for (const auto& table : tables_) {
        if (table.id == table_id) {
            return &table;
        }
    }
    return nullptr;
}

}  // namespace minidb::catalog
