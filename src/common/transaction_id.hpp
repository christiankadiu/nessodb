#pragma once

#include <cstdint>

namespace nessodb::common {

struct TransactionId {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return value != 0;
    }

    friend bool operator==(const TransactionId&, const TransactionId&) = default;
};

}  // namespace nessodb::common
