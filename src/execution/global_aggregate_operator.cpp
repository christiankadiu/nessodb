#include "execution/global_aggregate_operator.hpp"

#include "execution/expression_evaluator.hpp"

#include <stdexcept>
#include <utility>
#include <variant>

namespace minidb::execution {
namespace {

OperatorError expression_error(const ExpressionError& error) noexcept {
    const auto code =
        error.code == ExpressionErrorCode::integer_overflow
            ? OperatorErrorCode::integer_overflow
            : OperatorErrorCode::division_by_zero;
    return OperatorError{code, error.location};
}

}  // namespace

GlobalAggregateOperator::GlobalAggregateOperator(
    std::unique_ptr<RowOperator> child,
    std::vector<AggregateComputation> aggregates)
    : child_(std::move(child)), aggregates_(std::move(aggregates)) {
    if (!child_) {
        throw std::invalid_argument{
            "global aggregate operator requires a child"};
    }
    if (aggregates_.empty()) {
        throw std::invalid_argument{
            "global aggregate operator requires at least one aggregate"};
    }
    for (const auto& aggregate : aggregates_) {
        if (!aggregate.state) {
            throw std::invalid_argument{
                "aggregate computation requires a state"};
        }
        if (aggregate.arguments.size() !=
            aggregate.state->argument_count()) {
            throw std::invalid_argument{
                "aggregate argument count does not match its state"};
        }
        for (const auto& argument : aggregate.arguments) {
            if (argument.result_type() !=
                binder::BoundExpressionResultType::value) {
                throw std::invalid_argument{
                    "aggregate arguments must be value expressions"};
            }
        }
    }
}

OperatorResult GlobalAggregateOperator::next() {
    if (finished_) {
        return std::optional<RowBatch>{};
    }

    while (true) {
        auto input = child_->next();
        if (!input) {
            return std::unexpected(input.error());
        }
        if (!*input) {
            break;
        }

        for (const auto& row : (*input)->rows) {
            for (auto& aggregate : aggregates_) {
                std::vector<types::Value> arguments;
                arguments.reserve(aggregate.arguments.size());
                for (const auto& expression : aggregate.arguments) {
                    auto evaluated =
                        evaluate_expression(row.values, expression);
                    if (!evaluated) {
                        return std::unexpected(
                            expression_error(evaluated.error()));
                    }
                    auto* value = std::get_if<types::Value>(&*evaluated);
                    if (value == nullptr) {
                        throw std::logic_error{
                            "aggregate argument did not produce a value"};
                    }
                    arguments.push_back(std::move(*value));
                }
                auto accumulated = aggregate.state->accumulate(arguments);
                if (!accumulated) {
                    if (accumulated.error().code ==
                        AggregateErrorCode::invalid_argument_count) {
                        throw std::logic_error{
                            "aggregate state rejected its declared arity"};
                    }
                    return std::unexpected(OperatorError{
                        OperatorErrorCode::integer_overflow,
                        aggregate.location});
                }
            }
        }
    }

    storage::Row row;
    row.values.reserve(aggregates_.size());
    for (const auto& aggregate : aggregates_) {
        row.values.push_back(aggregate.state->finalize());
    }

    RowBatch output;
    output.rows.push_back(std::move(row));
    finished_ = true;
    return std::optional<RowBatch>{std::move(output)};
}

}  // namespace minidb::execution
