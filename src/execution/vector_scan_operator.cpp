#include "execution/vector_scan_operator.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace nessodb::execution {

VectorScanOperator::VectorScanOperator(std::vector<storage::Row> rows,
                                       std::size_t batch_size)
    : rows_(std::move(rows)), batch_size_(batch_size) {
    if (batch_size_ == 0) {
        throw std::invalid_argument{"row batch size must be greater than zero"};
    }
}

OperatorResult VectorScanOperator::next() {
    if (offset_ == rows_.size()) {
        return std::optional<RowBatch>{};
    }

    const std::size_t end =
        offset_ + std::min(batch_size_, rows_.size() - offset_);
    RowBatch batch;
    batch.rows.reserve(end - offset_);
    while (offset_ < end) {
        batch.rows.push_back(std::move(rows_[offset_]));
        ++offset_;
    }
    return std::optional<RowBatch>{std::move(batch)};
}

}  // namespace nessodb::execution
