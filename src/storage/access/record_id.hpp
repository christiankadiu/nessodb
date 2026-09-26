#pragma once

#include "common/page_id.hpp"
#include "storage/page/slotted_page.hpp"

namespace nessodb::storage {

struct RecordId {
    common::PageId page_id;
    SlotId slot_id;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return page_id.is_valid() && page_id.value != 0 && slot_id.is_valid();
    }

    friend bool operator==(const RecordId&, const RecordId&) = default;
};

}  // namespace nessodb::storage
