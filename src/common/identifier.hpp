#pragma once

#include <string_view>

namespace nessodb::common {

[[nodiscard]] constexpr char ascii_lower(char character) noexcept {
    if (character >= 'A' && character <= 'Z') {
        return static_cast<char>(character - 'A' + 'a');
    }
    return character;
}

[[nodiscard]] constexpr bool identifiers_equal(std::string_view left,
                                               std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

}  // namespace nessodb::common
