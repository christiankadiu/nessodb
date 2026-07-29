#include "engine/query.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <variant>

namespace {

using minidb::binder::BindError;
using minidb::engine::execute_query;
using minidb::sql::ParseError;

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

void test_query_result() {
    const auto result = execute_query("SELECT 1, 22;");

    expect(result.has_value(), "valid query is executed");
    if (!result) {
        return;
    }

    expect(result->values.size() == 2, "query returns two values");
    if (result->values.size() == 2) {
        expect(result->values[0] == std::int64_t{1}, "first value is returned");
        expect(result->values[1] == std::int64_t{22}, "second value is returned");
    }
}

void test_query_errors() {
    const auto parse_failure = execute_query("SELECT;");
    expect(!parse_failure, "invalid syntax is rejected");
    if (!parse_failure) {
        expect(std::holds_alternative<ParseError>(parse_failure.error()),
               "syntax failure preserves parse error");
    }

    const auto bind_failure = execute_query("SELECT 9223372036854775808;");
    expect(!bind_failure, "integer overflow is rejected");
    if (!bind_failure) {
        expect(std::holds_alternative<BindError>(bind_failure.error()),
               "overflow preserves bind error");
    }
}

}  // namespace

int main() {
    test_query_result();
    test_query_errors();

    if (failures != 0) {
        std::cerr << failures << " query assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
