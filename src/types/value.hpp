#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace minidb::types {

using Value = std::variant<std::int64_t, std::string>;

}  // namespace minidb::types
