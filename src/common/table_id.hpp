#pragma once

#include <cstdint>

namespace nessodb::common {

struct TableId {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return value != 0;
    }

    friend bool operator==(const TableId&, const TableId&) = default;
};

}  // namespace nessodb::common
