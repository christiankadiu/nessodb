#pragma once

#include <cstdint>

namespace minidb::common {

struct TableId {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return value != 0;
    }

    friend bool operator==(const TableId&, const TableId&) = default;
};

}  // namespace minidb::common
