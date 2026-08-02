#include "engine/database.hpp"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

void print_error(const minidb::engine::QueryError& error) {
    std::visit(
        [](const auto& detail) {
            if constexpr (requires { detail.location; }) {
                std::cerr << "Query error at " << detail.location.line << ':'
                          << detail.location.column << '\n';
            } else {
                std::cerr << "Storage error\n";
            }
        },
        error);
}

void print_error(const minidb::engine::DatabaseOpenError& error) {
    if (std::holds_alternative<minidb::catalog::CatalogError>(error)) {
        std::cerr << "Invalid database catalog\n";
    } else {
        std::cerr << "Cannot open database file\n";
    }
}

void print_value(const minidb::types::Value& value) {
    std::visit(
        [](const auto& item) {
            using Item = std::remove_cvref_t<decltype(item)>;
            if constexpr (std::is_same_v<Item, minidb::types::NullValue>) {
                std::cout << "NULL";
            } else {
                std::cout << item;
            }
        },
        value);
}

void print_result(const minidb::engine::QueryResult& result) {
    for (const auto& row : result.rows) {
        for (std::size_t index = 0; index < row.values.size(); ++index) {
            if (index != 0) {
                std::cout << " | ";
            }
            print_value(row.values[index]);
        }
        std::cout << '\n';
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: minidb <database-file> \"SQL statement\" "
                     "[\"SQL statement\" ...]\n";
        return 2;
    }

    const std::filesystem::path path{argv[1]};
    std::optional<minidb::engine::Database> database;
    if (path == ":memory:") {
        database.emplace();
    } else {
        std::error_code status_error;
        const bool exists = std::filesystem::exists(path, status_error);
        if (status_error) {
            std::cerr << "Cannot access database path\n";
            return 1;
        }

        auto opened = exists ? minidb::engine::Database::open(path)
                             : minidb::engine::Database::create(path);
        if (!opened) {
            print_error(opened.error());
            return 1;
        }
        database.emplace(std::move(*opened));
    }

    for (int index = 2; index < argc; ++index) {
        const auto result = database->execute(std::string_view{argv[index]});
        if (!result) {
            print_error(result.error());
            return 1;
        }

        print_result(*result);
    }
    return 0;
}
