#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace nessodb::types {

struct NullValue {
    friend bool operator==(const NullValue&, const NullValue&) = default;
};

using Value = std::variant<NullValue, std::int64_t, std::string>;

}  // namespace nessodb::types
