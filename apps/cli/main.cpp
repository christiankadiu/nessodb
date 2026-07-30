#include "engine/database.hpp"

#include <cstddef>
#include <iostream>
#include <string_view>
#include <type_traits>
#include <variant>

namespace {

void print_error(const minidb::engine::QueryError& error) {
    std::visit(
        [](const auto& detail) {
            std::cerr << "Query error at " << detail.location.line << ':' << detail.location.column
                      << '\n';
        },
        error);
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
    if (argc < 2) {
        std::cerr << "Usage: minidb \"SQL statement\" [\"SQL statement\" ...]\n";
        return 2;
    }

    minidb::engine::Database database;
    for (int index = 1; index < argc; ++index) {
        const auto result = database.execute(std::string_view{argv[index]});
        if (!result) {
            print_error(result.error());
            return 1;
        }

        print_result(*result);
    }
    return 0;
}
