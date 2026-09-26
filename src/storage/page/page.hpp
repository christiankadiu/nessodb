#pragma once

#include <array>
#include <cstddef>

namespace nessodb::storage {

inline constexpr std::size_t page_size = 4096;
using PageBuffer = std::array<std::byte, page_size>;

static_assert(sizeof(PageBuffer) == page_size);

}  // namespace nessodb::storage
