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

std::string_view transaction_error_message(
    nessodb::engine::TransactionExecutionErrorCode code) noexcept {
    using Code = nessodb::engine::TransactionExecutionErrorCode;
    switch (code) {
        case Code::transaction_already_active:
            return "transaction already active";
        case Code::no_active_transaction:
            return "no active transaction";
        case Code::lock_conflict:
            return "lock conflict";
        case Code::rollback_failed:
            return "rollback failed";
        case Code::transaction_state_error:
            return "invalid transaction state";
        case Code::ddl_not_supported:
            return "DDL is not supported inside an explicit transaction";
    }
    return "unknown transaction error";
}

void print_error(const nessodb::engine::QueryError& error) {
    std::visit(
        [](const auto& detail) {
            using Detail = std::remove_cvref_t<decltype(detail)>;
            if constexpr (requires { detail.location; }) {
                std::cerr << "Query error at " << detail.location.line << ':'
                          << detail.location.column << '\n';
            } else if constexpr (std::is_same_v<
                                     Detail,
                                     nessodb::engine::TransactionExecutionError>) {
                std::cerr << "Transaction error: "
                          << transaction_error_message(detail.code) << '\n';
            } else if constexpr (std::is_same_v<
                                     Detail,
                                     nessodb::engine::RecoveryError>) {
                std::cerr << "Recovery error\n";
            } else {
                std::cerr << "Storage error\n";
            }
        },
        error);
}

void print_error(const nessodb::engine::DatabaseOpenError& error) {
    if (std::holds_alternative<nessodb::catalog::CatalogError>(error)) {
        std::cerr << "Invalid database catalog\n";
    } else {
        std::cerr << "Cannot open database file\n";
    }
}

void print_value(const nessodb::types::Value& value) {
    std::visit(
        [](const auto& item) {
            using Item = std::remove_cvref_t<decltype(item)>;
            if constexpr (std::is_same_v<Item, nessodb::types::NullValue>) {
                std::cout << "NULL";
            } else {
                std::cout << item;
            }
        },
        value);
}

void print_result(const nessodb::engine::QueryResult& result) {
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
        std::cerr << "Usage: nessodb <database-file> \"SQL statement\" "
                     "[\"SQL statement\" ...]\n";
        return 2;
    }

    const std::filesystem::path path{argv[1]};
    std::optional<nessodb::engine::Database> database;
    if (path == ":memory:") {
        database.emplace();
    } else {
        std::error_code status_error;
        const bool exists = std::filesystem::exists(path, status_error);
        if (status_error) {
            std::cerr << "Cannot access database path\n";
            return 1;
        }

        auto opened = exists ? nessodb::engine::Database::open(path)
                             : nessodb::engine::Database::create(path);
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
