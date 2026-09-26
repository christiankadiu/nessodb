#pragma once

#include "types/value.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>

namespace nessodb::execution {

enum class AggregateErrorCode {
    invalid_argument_count,
    integer_overflow,
    type_mismatch,
};

struct AggregateError {
    AggregateErrorCode code;
};

using AggregateResult = std::expected<void, AggregateError>;

class AggregateState {
public:
    virtual ~AggregateState() = default;

    [[nodiscard]] virtual std::size_t argument_count() const noexcept = 0;
    [[nodiscard]] virtual std::unique_ptr<AggregateState> clone_empty()
        const = 0;
    [[nodiscard]] virtual std::size_t estimated_memory_usage()
        const noexcept = 0;
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
    [[nodiscard]] std::unique_ptr<AggregateState> clone_empty()
        const override;
    [[nodiscard]] std::size_t estimated_memory_usage()
        const noexcept override;
    [[nodiscard]] AggregateResult accumulate(
        std::span<const types::Value> arguments) override;
    [[nodiscard]] types::Value finalize() const override;

private:
    CountMode mode_;
    std::int64_t count_{};
};

enum class MinMaxMode {
    minimum,
    maximum,
};

class MinMaxAggregateState final : public AggregateState {
public:
    explicit MinMaxAggregateState(MinMaxMode mode) noexcept;

    [[nodiscard]] std::size_t argument_count() const noexcept override;
    [[nodiscard]] std::unique_ptr<AggregateState> clone_empty()
        const override;
    [[nodiscard]] std::size_t estimated_memory_usage()
        const noexcept override;
    [[nodiscard]] AggregateResult accumulate(
        std::span<const types::Value> arguments) override;
    [[nodiscard]] types::Value finalize() const override;

private:
    MinMaxMode mode_;
    std::optional<types::Value> value_;
};

class SumAggregateState final : public AggregateState {
public:
    [[nodiscard]] std::size_t argument_count() const noexcept override;
    [[nodiscard]] std::unique_ptr<AggregateState> clone_empty()
        const override;
    [[nodiscard]] std::size_t estimated_memory_usage()
        const noexcept override;
    [[nodiscard]] AggregateResult accumulate(
        std::span<const types::Value> arguments) override;
    [[nodiscard]] types::Value finalize() const override;

private:
    std::optional<std::int64_t> sum_;
};

}  // namespace nessodb::execution
