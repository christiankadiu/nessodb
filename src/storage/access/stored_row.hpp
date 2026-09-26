#pragma once

#include "storage/access/record_id.hpp"
#include "storage/access/row.hpp"

namespace nessodb::storage {

struct StoredRow {
    RecordId record_id;
    Row row;
};

}  // namespace nessodb::storage
