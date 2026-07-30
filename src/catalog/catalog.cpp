#include "catalog/catalog.hpp"

#include "common/identifier.hpp"

#include <utility>

namespace minidb::catalog {

std::expected<const TableSchema*, CatalogError> Catalog::create_table(TableSchema schema) {
    if (find_table(schema.name) != nullptr) {
        return std::unexpected(CatalogError::table_already_exists);
    }

    schema.id = common::TableId{next_table_id_};
    ++next_table_id_;
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

}  // namespace minidb::catalog
