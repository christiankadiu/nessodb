#pragma once

#include "types/value.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace minidb::execution {

enum class AggregateErrorCode {
    invalid_argument_count,
    integer_overflow,
};

struct AggregateError {
    AggregateErrorCode code;
};

using AggregateResult = std::expected<void, AggregateError>;

class AggregateState {
public:
    virtual ~AggregateState() = default;

    [[nodiscard]] virtual std::size_t argument_count() const noexcept = 0;
    [[nodiscard]] virtual AggregateResult accumulate(
        std::span<const types::Value> arguments) = 0;
    [[nodiscard]] virtual types::Value finalize() const = 0;
};

enum class CountMode {
    all_rows,
    non_null_values,
};

class CountAggregateState final : public AggregateState {
public:
    explicit CountAggregateState(CountMode mode) noexcept;

    [[nodiscard]] std::size_t argument_count() const noexcept override;
    [[nodiscard]] AggregateResult accumulate(
        std::span<const types::Value> arguments) override;
    [[nodiscard]] types::Value finalize() const override;

private:
    CountMode mode_;
    std::int64_t count_{};
};

}  // namespace minidb::execution
