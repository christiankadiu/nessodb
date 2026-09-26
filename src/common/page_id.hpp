#pragma once

#include <cstdint>
#include <limits>

namespace nessodb::common {

struct PageId {
    static constexpr std::uint64_t invalid_value = std::numeric_limits<std::uint64_t>::max();

    std::uint64_t value{invalid_value};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return value != invalid_value;
    }

    friend bool operator==(const PageId&, const PageId&) = default;
};

}  // namespace nessodb::common
