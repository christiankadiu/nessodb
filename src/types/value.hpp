#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace minidb::types {

struct NullValue {
    friend bool operator==(const NullValue&, const NullValue&) = default;
};

using Value = std::variant<NullValue, std::int64_t, std::string>;

}  // namespace minidb::types
