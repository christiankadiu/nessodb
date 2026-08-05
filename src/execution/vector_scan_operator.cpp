#include "execution/vector_scan_operator.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace minidb::execution {

VectorScanOperator::VectorScanOperator(std::vector<storage::Row> rows,
                                       std::size_t batch_size)
    : rows_(std::move(rows)), batch_size_(batch_size) {
    if (batch_size_ == 0) {
        throw std::invalid_argument{"row batch size must be greater than zero"};
    }
}

std::optional<RowBatch> VectorScanOperator::next() {
    if (offset_ == rows_.size()) {
        return std::nullopt;
    }

    const std::size_t end = std::min(offset_ + batch_size_, rows_.size());
    RowBatch batch;
    batch.rows.reserve(end - offset_);
    while (offset_ < end) {
        batch.rows.push_back(std::move(rows_[offset_]));
        ++offset_;
    }
    return batch;
}

}  // namespace minidb::execution
