#include "engine/query.hpp"

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

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: minidb \"SELECT ...\"\n";
        return 2;
    }

    const auto result = minidb::engine::execute_query(std::string_view{argv[1]});
    if (!result) {
        print_error(result.error());
        return 1;
    }

    for (const auto& row : result->rows) {
        for (std::size_t index = 0; index < row.values.size(); ++index) {
            if (index != 0) {
                std::cout << " | ";
            }
            print_value(row.values[index]);
        }
        std::cout << '\n';
    }
    return 0;
}
