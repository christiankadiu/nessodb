#include "engine/query.hpp"

#include <cstddef>
#include <iostream>
#include <string_view>
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

    for (std::size_t index = 0; index < result->values.size(); ++index) {
        if (index != 0) {
            std::cout << " | ";
        }
        std::cout << result->values[index];
    }
    std::cout << '\n';
    return 0;
}
