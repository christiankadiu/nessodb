#include "binder/binder.hpp"

#include "catalog/catalog.hpp"
#include "common/identifier.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace minidb::binder {
namespace {

std::expected<types::Value, BindError> bind_integer(const sql::LiteralExpression& expression) {
    std::int64_t value{};
    const char* const begin = expression.text.data();
    const char* const end = begin + expression.text.size();
    const auto result = std::from_chars(begin, end, value);

    if (result.ec == std::errc::result_out_of_range) {
        return std::unexpected(
            BindError{BindErrorCode::integer_out_of_range, expression.location});
    }
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::unexpected(
            BindError{BindErrorCode::invalid_integer_literal, expression.location});
    }
    return types::Value{value};
}

std::optional<std::int64_t> bind_minimum_integer(
    const sql::LiteralExpression& expression) noexcept {
    if (expression.type != sql::LiteralType::integer) {
        return std::nullopt;
    }

    std::uint64_t magnitude{};
    const char* const begin = expression.text.data();
    const char* const end = begin + expression.text.size();
    const auto result = std::from_chars(begin, end, magnitude);
    constexpr std::uint64_t minimum_magnitude =
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()) +
        std::uint64_t{1};
    if (result.ec != std::errc{} || result.ptr != end ||
        magnitude != minimum_magnitude) {
        return std::nullopt;
    }
    return std::numeric_limits<std::int64_t>::min();
}

std::expected<std::size_t, BindError> bind_row_count(
    std::string_view count, sql::SourceLocation location) {
    std::size_t value{};
    const char* const begin = count.data();
    const char* const end = begin + count.size();
    const auto result = std::from_chars(begin, end, value);

    if (result.ec == std::errc::result_out_of_range) {
        return std::unexpected(
            BindError{BindErrorCode::integer_out_of_range, location});
    }
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::unexpected(BindError{
            BindErrorCode::invalid_integer_literal, location});
    }
    return value;
}

std::expected<types::Value, BindError> bind_string(const sql::LiteralExpression& expression) {
    const std::string_view text = expression.text;
    if (text.size() < 2 || text.front() != '\'' || text.back() != '\'') {
        return std::unexpected(
            BindError{BindErrorCode::invalid_string_literal, expression.location});
    }

    std::string value;
    value.reserve(text.size() - 2);

    for (std::size_t index = 1; index + 1 < text.size(); ++index) {
        if (text[index] != '\'') {
            value.push_back(text[index]);
            continue;
        }
        if (index + 1 >= text.size() - 1 || text[index + 1] != '\'') {
            return std::unexpected(
                BindError{BindErrorCode::invalid_string_literal, expression.location});
        }
        value.push_back('\'');
        ++index;
    }

    return types::Value{std::move(value)};
}

std::expected<BoundLiteralExpression, BindError> bind_literal(
    const sql::LiteralExpression& expression) {
    types::Value value{types::NullValue{}};
    if (expression.type != sql::LiteralType::null) {
        auto bound_value = expression.type == sql::LiteralType::integer
                               ? bind_integer(expression)
                               : bind_string(expression);
        if (!bound_value) {
            return std::unexpected(bound_value.error());
        }
        value = std::move(*bound_value);
    }
    return BoundLiteralExpression{std::move(value), expression.location};
}

bool value_matches_type(const types::Value& value, types::LogicalType type) noexcept {
    if (std::holds_alternative<types::NullValue>(value)) {
        return true;
    }
    if (type == types::LogicalType::integer) {
        return std::holds_alternative<std::int64_t>(value);
    }
    return std::holds_alternative<std::string>(value);
}

std::size_t find_column_index(const catalog::TableSchema& table,
                              std::string_view name) noexcept {
    std::size_t index = 0;
    while (index < table.columns.size() &&
           !common::identifiers_equal(table.columns[index].name, name)) {
        ++index;
    }
    return index;
}

struct BindingTable {
    const catalog::TableSchema* schema;
    std::string_view visible_name;
    std::size_t column_offset;
};

struct ResolvedColumn {
    std::size_t column_index;
    types::LogicalType type;
    std::string_view name;
};

std::expected<ResolvedColumn, BindError> resolve_column(
    std::span<const BindingTable> tables,
    const sql::ColumnReferenceExpression& column) {
    const BindingTable* match = nullptr;
    std::size_t local_index = 0;
    for (const auto& table : tables) {
        if (column.qualifier &&
            !common::identifiers_equal(column.qualifier->name,
                                       table.visible_name)) {
            continue;
        }
        const auto index = find_column_index(*table.schema, column.name);
        if (index == table.schema->columns.size()) {
            continue;
        }
        if (match != nullptr) {
            return std::unexpected(BindError{
                BindErrorCode::ambiguous_column, column.location});
        }
        match = &table;
        local_index = index;
    }
    if (match == nullptr) {
        if (column.qualifier) {
            bool qualifier_exists = false;
            for (const auto& table : tables) {
                qualifier_exists = qualifier_exists ||
                    common::identifiers_equal(
                        column.qualifier->name, table.visible_name);
            }
            if (!qualifier_exists) {
                return std::unexpected(BindError{
                    BindErrorCode::table_not_found,
                    column.qualifier->location});
            }
        }
        return std::unexpected(BindError{
            BindErrorCode::column_not_found, column.location});
    }
    const auto& schema_column = match->schema->columns[local_index];
    return ResolvedColumn{
        match->column_offset + local_index, schema_column.type,
        schema_column.name};
}

std::expected<std::size_t, BindError> resolve_column(
    const catalog::TableSchema& table,
    const sql::ColumnReferenceExpression& column) {
    const BindingTable binding{&table, table.name, 0};
    auto resolved = resolve_column(
        std::span<const BindingTable>{&binding, 1}, column);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    return resolved->column_index;
}

BoundComparisonOperator bind_comparison_operator(
    sql::ComparisonOperator comparison) noexcept {
    switch (comparison) {
        case sql::ComparisonOperator::equal:
            return BoundComparisonOperator::equal;
        case sql::ComparisonOperator::not_equal:
            return BoundComparisonOperator::not_equal;
        case sql::ComparisonOperator::less:
            return BoundComparisonOperator::less;
        case sql::ComparisonOperator::less_equal:
            return BoundComparisonOperator::less_equal;
        case sql::ComparisonOperator::greater:
            return BoundComparisonOperator::greater;
        case sql::ComparisonOperator::greater_equal:
            return BoundComparisonOperator::greater_equal;
    }
    return BoundComparisonOperator::equal;
}

BoundLogicalOperator bind_logical_operator(
    sql::LogicalOperator operation) noexcept {
    switch (operation) {
        case sql::LogicalOperator::conjunction:
            return BoundLogicalOperator::conjunction;
        case sql::LogicalOperator::disjunction:
            return BoundLogicalOperator::disjunction;
    }
    return BoundLogicalOperator::conjunction;
}

BoundUnaryArithmeticOperator bind_unary_arithmetic_operator(
    sql::UnaryArithmeticOperator operation) noexcept {
    return operation == sql::UnaryArithmeticOperator::plus
               ? BoundUnaryArithmeticOperator::plus
               : BoundUnaryArithmeticOperator::minus;
}

BoundBinaryArithmeticOperator bind_binary_arithmetic_operator(
    sql::BinaryArithmeticOperator operation) noexcept {
    switch (operation) {
        case sql::BinaryArithmeticOperator::addition:
            return BoundBinaryArithmeticOperator::addition;
        case sql::BinaryArithmeticOperator::subtraction:
            return BoundBinaryArithmeticOperator::subtraction;
        case sql::BinaryArithmeticOperator::multiplication:
            return BoundBinaryArithmeticOperator::multiplication;
        case sql::BinaryArithmeticOperator::division:
            return BoundBinaryArithmeticOperator::division;
    }
    return BoundBinaryArithmeticOperator::addition;
}

const sql::SourceLocation& expression_location(
    const sql::Expression& expression) noexcept {
    if (const auto* literal =
            std::get_if<sql::LiteralExpression>(&expression.node)) {
        return literal->location;
    }
    if (const auto* column =
            std::get_if<sql::ColumnReferenceExpression>(&expression.node)) {
        return column->location;
    }
    if (const auto* unary =
            std::get_if<sql::UnaryArithmeticExpression>(&expression.node)) {
        return unary->location;
    }
    if (const auto* binary =
            std::get_if<sql::BinaryArithmeticExpression>(&expression.node)) {
        return binary->location;
    }
    if (const auto* comparison =
            std::get_if<sql::ComparisonExpression>(&expression.node)) {
        return comparison->location;
    }
    if (const auto* null_test =
            std::get_if<sql::NullTestExpression>(&expression.node)) {
        return null_test->location;
    }
    if (const auto* logical =
            std::get_if<sql::LogicalExpression>(&expression.node)) {
        return logical->location;
    }
    if (const auto* negation =
            std::get_if<sql::NegationExpression>(&expression.node)) {
        return negation->location;
    }
    return std::get<sql::FunctionCallExpression>(expression.node).location;
}

bool is_integer_expression(const BoundExpression& expression) noexcept {
    if (expression.result_type() != BoundExpressionResultType::value) {
        return false;
    }
    const auto type = expression.value_type();
    return !type || *type == types::LogicalType::integer;
}

std::expected<BoundExpression, BindError> bind_expression(
    const sql::Expression& expression,
    std::span<const BindingTable> tables) {
    if (const auto* literal =
            std::get_if<sql::LiteralExpression>(&expression.node)) {
        auto bound = bind_literal(*literal);
        if (!bound) {
            return std::unexpected(bound.error());
        }
        return BoundExpression{std::move(*bound)};
    }

    if (const auto* column =
            std::get_if<sql::ColumnReferenceExpression>(&expression.node)) {
        if (tables.empty()) {
            return std::unexpected(BindError{
                BindErrorCode::column_requires_table, column->location});
        }
        auto resolved = resolve_column(tables, *column);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        return BoundExpression{BoundColumnReferenceExpression{
            resolved->column_index, column->location, resolved->type}};
    }

    if (const auto* unary =
            std::get_if<sql::UnaryArithmeticExpression>(&expression.node)) {
        if (!unary->operand) {
            throw std::logic_error{
                "unary arithmetic expression requires an operand"};
        }
        if (unary->operation == sql::UnaryArithmeticOperator::minus) {
            const auto* literal =
                std::get_if<sql::LiteralExpression>(&unary->operand->node);
            if (literal != nullptr) {
                const auto minimum = bind_minimum_integer(*literal);
                if (minimum) {
                    return BoundExpression{BoundLiteralExpression{
                        types::Value{*minimum}, literal->location}};
                }
            }
        }
        auto operand = bind_expression(*unary->operand, tables);
        if (!operand) {
            return std::unexpected(operand.error());
        }
        if (!is_integer_expression(*operand)) {
            return std::unexpected(BindError{
                BindErrorCode::type_mismatch,
                expression_location(*unary->operand)});
        }
        return BoundExpression{BoundUnaryArithmeticExpression{
            bind_unary_arithmetic_operator(unary->operation),
            std::make_unique<BoundExpression>(std::move(*operand)),
            unary->location}};
    }

    if (const auto* binary =
            std::get_if<sql::BinaryArithmeticExpression>(&expression.node)) {
        if (!binary->left || !binary->right) {
            throw std::logic_error{
                "binary arithmetic expression requires two operands"};
        }
        auto left = bind_expression(*binary->left, tables);
        if (!left) {
            return std::unexpected(left.error());
        }
        if (!is_integer_expression(*left)) {
            return std::unexpected(BindError{
                BindErrorCode::type_mismatch,
                expression_location(*binary->left)});
        }
        auto right = bind_expression(*binary->right, tables);
        if (!right) {
            return std::unexpected(right.error());
        }
        if (!is_integer_expression(*right)) {
            return std::unexpected(BindError{
                BindErrorCode::type_mismatch,
                expression_location(*binary->right)});
        }
        return BoundExpression{BoundBinaryArithmeticExpression{
            bind_binary_arithmetic_operator(binary->operation),
            std::make_unique<BoundExpression>(std::move(*left)),
            std::make_unique<BoundExpression>(std::move(*right)),
            binary->location}};
    }

    if (const auto* comparison =
            std::get_if<sql::ComparisonExpression>(&expression.node)) {
        if (!comparison->left || !comparison->right) {
            throw std::logic_error{
                "comparison expression requires two operands"};
        }
        auto left = bind_expression(*comparison->left, tables);
        if (!left) {
            return std::unexpected(left.error());
        }
        auto right = bind_expression(*comparison->right, tables);
        if (!right) {
            return std::unexpected(right.error());
        }
        if (left->result_type() != BoundExpressionResultType::value ||
            right->result_type() != BoundExpressionResultType::value) {
            return std::unexpected(BindError{
                BindErrorCode::type_mismatch,
                expression_location(*comparison->right)});
        }
        const auto left_type = left->value_type();
        const auto right_type = right->value_type();
        if (left_type && right_type && *left_type != *right_type) {
            return std::unexpected(BindError{
                BindErrorCode::type_mismatch,
                expression_location(*comparison->right)});
        }
        return BoundExpression{BoundComparisonExpression{
            bind_comparison_operator(comparison->comparison),
            std::make_unique<BoundExpression>(std::move(*left)),
            std::make_unique<BoundExpression>(std::move(*right))}};
    }

    if (const auto* null_test =
            std::get_if<sql::NullTestExpression>(&expression.node)) {
        if (!null_test->operand) {
            throw std::logic_error{
                "null test expression requires an operand"};
        }
        auto operand = bind_expression(*null_test->operand, tables);
        if (!operand) {
            return std::unexpected(operand.error());
        }
        if (operand->result_type() != BoundExpressionResultType::value) {
            return std::unexpected(BindError{
                BindErrorCode::type_mismatch,
                expression_location(*null_test->operand)});
        }
        return BoundExpression{BoundNullTestExpression{
            std::make_unique<BoundExpression>(std::move(*operand)),
            null_test->negated}};
    }

    if (const auto* negation =
            std::get_if<sql::NegationExpression>(&expression.node)) {
        if (!negation->operand) {
            throw std::logic_error{
                "negation expression requires an operand"};
        }
        auto operand = bind_expression(*negation->operand, tables);
        if (!operand) {
            return std::unexpected(operand.error());
        }
        if (operand->result_type() != BoundExpressionResultType::boolean) {
            return std::unexpected(BindError{
                BindErrorCode::type_mismatch,
                expression_location(*negation->operand)});
        }
        return BoundExpression{BoundNegationExpression{
            std::make_unique<BoundExpression>(std::move(*operand))}};
    }

    if (std::holds_alternative<sql::FunctionCallExpression>(
            expression.node)) {
        return std::unexpected(BindError{
            BindErrorCode::invalid_function_arguments,
            expression_location(expression)});
    }

    const auto& logical = std::get<sql::LogicalExpression>(expression.node);
    if (!logical.left || !logical.right) {
        throw std::logic_error{"logical expression requires two operands"};
    }
    auto left = bind_expression(*logical.left, tables);
    if (!left) {
        return std::unexpected(left.error());
    }
    if (left->result_type() != BoundExpressionResultType::boolean) {
        return std::unexpected(BindError{
            BindErrorCode::type_mismatch,
            expression_location(*logical.left)});
    }
    auto right = bind_expression(*logical.right, tables);
    if (!right) {
        return std::unexpected(right.error());
    }
    if (right->result_type() != BoundExpressionResultType::boolean) {
        return std::unexpected(BindError{
            BindErrorCode::type_mismatch,
            expression_location(*logical.right)});
    }
    return BoundExpression{BoundLogicalExpression{
        bind_logical_operator(logical.operation),
        std::make_unique<BoundExpression>(std::move(*left)),
        std::make_unique<BoundExpression>(std::move(*right))}};
}

std::expected<BoundExpression, BindError> bind_expression(
    const sql::Expression& expression,
    const catalog::TableSchema* table,
    std::string_view visible_table_name = {}) {
    if (table == nullptr) {
        return bind_expression(expression,
                               std::span<const BindingTable>{});
    }
    if (visible_table_name.empty()) {
        visible_table_name = table->name;
    }
    const BindingTable binding{table, visible_table_name, 0};
    return bind_expression(
        expression, std::span<const BindingTable>{&binding, 1});
}

bool same_value_expression(const BoundExpression& left,
                           const BoundExpression& right) noexcept {
    if (left.node.index() != right.node.index()) {
        return false;
    }
    if (const auto* left_literal =
            std::get_if<BoundLiteralExpression>(&left.node)) {
        return left_literal->value ==
               std::get<BoundLiteralExpression>(right.node).value;
    }
    if (const auto* left_column =
            std::get_if<BoundColumnReferenceExpression>(&left.node)) {
        return left_column->column_index ==
               std::get<BoundColumnReferenceExpression>(right.node)
                   .column_index;
    }
    if (const auto* left_unary =
            std::get_if<BoundUnaryArithmeticExpression>(&left.node)) {
        const auto& right_unary =
            std::get<BoundUnaryArithmeticExpression>(right.node);
        return left_unary->operation == right_unary.operation &&
               left_unary->operand && right_unary.operand &&
               same_value_expression(*left_unary->operand,
                                     *right_unary.operand);
    }
    if (const auto* left_binary =
            std::get_if<BoundBinaryArithmeticExpression>(&left.node)) {
        const auto& right_binary =
            std::get<BoundBinaryArithmeticExpression>(right.node);
        return left_binary->operation == right_binary.operation &&
               left_binary->left && left_binary->right &&
               right_binary.left && right_binary.right &&
               same_value_expression(*left_binary->left,
                                     *right_binary.left) &&
               same_value_expression(*left_binary->right,
                                     *right_binary.right);
    }
    return false;
}

std::optional<types::LogicalType> aggregate_result_type(
    const BoundAggregateExpression& aggregate) noexcept {
    if (aggregate.function == BoundAggregateFunction::count ||
        aggregate.function == BoundAggregateFunction::sum) {
        return types::LogicalType::integer;
    }
    if (aggregate.arguments.empty()) {
        return std::nullopt;
    }
    return aggregate.arguments.front().value_type();
}

std::string_view column_name_at(
    std::span<const BindingTable> tables,
    std::size_t column_index) {
    for (const auto& table : tables) {
        if (column_index >= table.column_offset &&
            column_index - table.column_offset <
                table.schema->columns.size()) {
            return table.schema
                ->columns[column_index - table.column_offset].name;
        }
    }
    throw std::logic_error{
        "bound column index does not belong to a table scope"};
}

}  // namespace

std::expected<BoundSelectStatement, BindError> bind_select_statement(
    const sql::SelectStatement& statement, const catalog::Catalog& catalog) {
    const catalog::TableSchema* table = nullptr;
    std::vector<BindingTable> tables;
    if (statement.from) {
        table = catalog.find_table(statement.from->name);
        if (table == nullptr) {
            return std::unexpected(
                BindError{BindErrorCode::table_not_found,
                          statement.from->location});
        }
        const auto visible_name = statement.from->alias
                                      ? statement.from->alias->name
                                      : std::string_view{table->name};
        tables.push_back(BindingTable{table, visible_name, 0});
    }

    if (statement.select_all_columns) {
        if (table == nullptr) {
            return std::unexpected(BindError{BindErrorCode::table_not_found, {}});
        }
    }

    BoundSelectStatement bound_statement;
    bound_statement.distinct = statement.distinct;
    if (table != nullptr) {
        bound_statement.table_id = table->id;
    }
    bound_statement.joins.reserve(statement.joins.size());
    for (const auto& join : statement.joins) {
        if (tables.empty()) {
            return std::unexpected(BindError{
                BindErrorCode::table_not_found, join.table.location});
        }
        const auto* joined_table = catalog.find_table(join.table.name);
        if (joined_table == nullptr) {
            return std::unexpected(BindError{
                BindErrorCode::table_not_found, join.table.location});
        }
        const auto visible_name = join.table.alias
                                      ? join.table.alias->name
                                      : std::string_view{joined_table->name};
        for (const auto& existing : tables) {
            if (common::identifiers_equal(existing.visible_name,
                                          visible_name)) {
                return std::unexpected(BindError{
                    BindErrorCode::duplicate_table,
                    join.table.alias ? join.table.alias->location
                                     : join.table.location});
            }
        }
        const auto& previous = tables.back();
        const auto offset = previous.column_offset +
                            previous.schema->columns.size();
        tables.push_back(BindingTable{
            joined_table, visible_name, offset});
        auto condition = bind_expression(join.condition, tables);
        if (!condition) {
            return std::unexpected(condition.error());
        }
        if (condition->result_type() !=
            BoundExpressionResultType::boolean) {
            return std::unexpected(BindError{
                BindErrorCode::type_mismatch,
                expression_location(join.condition)});
        }
        bound_statement.joins.push_back(BoundJoin{
            joined_table->id, std::move(*condition)});
    }
    bound_statement.group_by.reserve(statement.group_by.size());
    for (const auto& expression : statement.group_by) {
        auto group_key = bind_expression(expression, tables);
        if (!group_key) {
            return std::unexpected(group_key.error());
        }
        if (group_key->result_type() !=
            BoundExpressionResultType::value) {
            return std::unexpected(BindError{
                BindErrorCode::type_mismatch,
                expression_location(expression)});
        }
        bound_statement.group_by.push_back(std::move(*group_key));
    }
    const bool grouped = !bound_statement.group_by.empty();
    if (statement.select_all_columns && grouped) {
        return std::unexpected(BindError{
            BindErrorCode::column_not_grouped,
            expression_location(statement.group_by.front())});
    }
    bound_statement.expressions.reserve(statement.expressions.size());
    bound_statement.result_column_names.reserve(
        statement.select_all_columns && table != nullptr
            ? table->columns.size()
            : statement.expressions.size());

    if (statement.select_all_columns) {
        for (const auto& binding : tables) {
            for (const auto& column : binding.schema->columns) {
                bound_statement.result_column_names.push_back(column.name);
            }
        }
    }

    bool has_aggregate = false;
    bool has_scalar = false;
    for (const auto& expression : statement.expressions) {
        if (const auto* function =
                std::get_if<sql::FunctionCallExpression>(&expression.node)) {
            std::optional<BoundAggregateFunction> aggregate_function;
            std::string_view default_name;
            if (common::identifiers_equal(function->name, "count")) {
                aggregate_function = BoundAggregateFunction::count;
                default_name = "count";
            } else if (common::identifiers_equal(function->name, "min")) {
                aggregate_function = BoundAggregateFunction::minimum;
                default_name = "min";
            } else if (common::identifiers_equal(function->name, "max")) {
                aggregate_function = BoundAggregateFunction::maximum;
                default_name = "max";
            } else if (common::identifiers_equal(function->name, "sum")) {
                aggregate_function = BoundAggregateFunction::sum;
                default_name = "sum";
            }
            if (!aggregate_function) {
                return std::unexpected(BindError{
                    BindErrorCode::function_not_found,
                    function->location});
            }
            const bool count = *aggregate_function ==
                               BoundAggregateFunction::count;
            if ((!grouped && has_scalar) ||
                (!count && function->star_argument) ||
                (function->star_argument && !function->arguments.empty()) ||
                (!function->star_argument &&
                 function->arguments.size() != 1)) {
                return std::unexpected(BindError{
                    !grouped && has_scalar
                        ? BindErrorCode::mixed_aggregate_and_scalar
                        : BindErrorCode::invalid_function_arguments,
                    function->location});
            }

            BoundAggregateExpression aggregate{
                *aggregate_function, {}, function->location};
            if (!function->star_argument) {
                auto argument = bind_expression(
                    *function->arguments.front(), tables);
                if (!argument) {
                    return std::unexpected(argument.error());
                }
                if (argument->result_type() !=
                    BoundExpressionResultType::value) {
                    return std::unexpected(BindError{
                        BindErrorCode::type_mismatch,
                        expression_location(*function->arguments.front())});
                }
                if (*aggregate_function == BoundAggregateFunction::sum &&
                    argument->value_type() &&
                    *argument->value_type() !=
                        types::LogicalType::integer) {
                    return std::unexpected(BindError{
                        BindErrorCode::type_mismatch,
                        expression_location(*function->arguments.front())});
                }
                aggregate.arguments.push_back(std::move(*argument));
            }
            const auto output_type = aggregate_result_type(aggregate);
            const auto aggregate_index =
                bound_statement.aggregates.size();
            bound_statement.aggregates.push_back(std::move(aggregate));
            if (grouped) {
                bound_statement.expressions.emplace_back(
                    BoundColumnReferenceExpression{
                        bound_statement.group_by.size() + aggregate_index,
                        function->location, output_type});
            }
            bound_statement.result_column_names.emplace_back(
                expression.alias ? expression.alias->name : default_name);
            has_aggregate = true;
            continue;
        }
        if (!grouped && has_aggregate) {
            return std::unexpected(BindError{
                BindErrorCode::mixed_aggregate_and_scalar,
                expression_location(expression)});
        }
        has_scalar = true;
        auto bound_expression = bind_expression(expression, tables);
        if (!bound_expression) {
            return std::unexpected(bound_expression.error());
        }
        if (bound_expression->result_type() !=
            BoundExpressionResultType::value) {
            return std::unexpected(BindError{
                BindErrorCode::type_mismatch,
                expression_location(expression)});
        }
        if (expression.alias) {
            bound_statement.result_column_names.emplace_back(
                expression.alias->name);
        } else if (const auto* column =
                       std::get_if<BoundColumnReferenceExpression>(
                           &bound_expression->node)) {
            bound_statement.result_column_names.push_back(
                std::string{column_name_at(
                    tables, column->column_index)});
        } else {
            bound_statement.result_column_names.emplace_back("?column?");
        }
        if (grouped) {
            std::size_t group_index = 0;
            while (group_index < bound_statement.group_by.size() &&
                   !same_value_expression(
                       *bound_expression,
                       bound_statement.group_by[group_index])) {
                ++group_index;
            }
            if (group_index == bound_statement.group_by.size()) {
                return std::unexpected(BindError{
                    BindErrorCode::column_not_grouped,
                    expression_location(expression)});
            }
            bound_statement.expressions.emplace_back(
                BoundColumnReferenceExpression{
                    group_index, expression_location(expression),
                    bound_statement.group_by[group_index].value_type()});
        } else {
            bound_statement.expressions.push_back(
                std::move(*bound_expression));
        }
    }

    if (statement.where) {
        if (table == nullptr) {
            return std::unexpected(
                BindError{BindErrorCode::column_requires_table,
                          expression_location(*statement.where)});
        }
        auto expression = bind_expression(*statement.where, tables);
        if (!expression) {
            return std::unexpected(expression.error());
        }
        if (expression->result_type() !=
            BoundExpressionResultType::boolean) {
            return std::unexpected(BindError{
                BindErrorCode::type_mismatch,
                expression_location(*statement.where)});
        }
        bound_statement.where = std::move(*expression);
    }

    if (!statement.order_by.empty() && table == nullptr) {
        return std::unexpected(
            BindError{BindErrorCode::column_requires_table,
                      statement.order_by.front().column.location});
    }
    bound_statement.order_by.reserve(statement.order_by.size());
    for (const auto& term : statement.order_by) {
        auto resolved = resolve_column(tables, term.column);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        const auto direction =
            term.direction == sql::OrderDirection::ascending
                ? BoundOrderDirection::ascending
                : BoundOrderDirection::descending;
        if (grouped) {
            std::size_t group_index = 0;
            while (group_index < bound_statement.group_by.size()) {
                const auto* column =
                    std::get_if<BoundColumnReferenceExpression>(
                        &bound_statement.group_by[group_index].node);
                if (column != nullptr &&
                    column->column_index == resolved->column_index) {
                    break;
                }
                ++group_index;
            }
            if (group_index == bound_statement.group_by.size()) {
                return std::unexpected(BindError{
                    BindErrorCode::column_not_grouped,
                    term.column.location});
            }
            bound_statement.order_by.push_back(
                BoundOrderByTerm{group_index, direction});
            continue;
        }
        bound_statement.order_by.push_back(
            BoundOrderByTerm{resolved->column_index, direction});
    }

    if (statement.limit) {
        auto limit = bind_row_count(statement.limit->count,
                                    statement.limit->location);
        if (!limit) {
            return std::unexpected(limit.error());
        }
        bound_statement.limit = *limit;
    }
    if (statement.offset) {
        auto offset = bind_row_count(statement.offset->count,
                                     statement.offset->location);
        if (!offset) {
            return std::unexpected(offset.error());
        }
        bound_statement.offset = *offset;
    }

    return bound_statement;
}

std::expected<BoundCreateTableStatement, BindError> bind_create_table_statement(
    const sql::CreateTableStatement& statement) {
    BoundCreateTableStatement bound_statement{
        std::string{statement.table_name}, statement.table_location, {}};
    bound_statement.columns.reserve(statement.columns.size());

    for (const auto& column : statement.columns) {
        for (const auto& existing_column : bound_statement.columns) {
            if (common::identifiers_equal(column.name, existing_column.name)) {
                return std::unexpected(
                    BindError{BindErrorCode::duplicate_column, column.location});
            }
        }

        const types::LogicalType type = column.type == sql::ColumnType::integer
                                            ? types::LogicalType::integer
                                            : types::LogicalType::text;
        bound_statement.columns.push_back(
            BoundColumnDefinition{std::string{column.name}, type, column.location});
    }

    return bound_statement;
}

std::expected<BoundInsertStatement, BindError> bind_insert_statement(
    const sql::InsertStatement& statement, const catalog::Catalog& catalog) {
    const catalog::TableSchema* table = catalog.find_table(statement.table_name);
    if (table == nullptr) {
        return std::unexpected(
            BindError{BindErrorCode::table_not_found, statement.table_location});
    }
    if (statement.values.size() != table->columns.size()) {
        return std::unexpected(
            BindError{BindErrorCode::column_count_mismatch, statement.table_location});
    }

    BoundInsertStatement bound_statement{table->id, {}};
    bound_statement.values.reserve(statement.values.size());
    for (std::size_t index = 0; index < statement.values.size(); ++index) {
        auto expression = bind_literal(statement.values[index]);
        if (!expression) {
            return std::unexpected(expression.error());
        }
        if (!value_matches_type(expression->value, table->columns[index].type)) {
            return std::unexpected(
                BindError{BindErrorCode::type_mismatch, expression->location});
        }
        bound_statement.values.push_back(std::move(expression->value));
    }
    return bound_statement;
}

std::expected<BoundDeleteStatement, BindError> bind_delete_statement(
    const sql::DeleteStatement& statement, const catalog::Catalog& catalog) {
    const catalog::TableSchema* table = catalog.find_table(statement.from.name);
    if (table == nullptr) {
        return std::unexpected(
            BindError{BindErrorCode::table_not_found, statement.from.location});
    }

    BoundDeleteStatement bound_statement{table->id, std::nullopt};
    if (statement.where) {
        auto expression = bind_expression(*statement.where, table);
        if (!expression) {
            return std::unexpected(expression.error());
        }
        if (expression->result_type() !=
            BoundExpressionResultType::boolean) {
            return std::unexpected(BindError{
                BindErrorCode::type_mismatch,
                expression_location(*statement.where)});
        }
        bound_statement.where = std::move(*expression);
    }
    return bound_statement;
}

std::expected<BoundUpdateStatement, BindError> bind_update_statement(
    const sql::UpdateStatement& statement, const catalog::Catalog& catalog) {
    const catalog::TableSchema* table = catalog.find_table(statement.table.name);
    if (table == nullptr) {
        return std::unexpected(
            BindError{BindErrorCode::table_not_found,
                      statement.table.location});
    }

    BoundUpdateStatement bound_statement{table->id, {}, std::nullopt};
    bound_statement.assignments.reserve(statement.assignments.size());
    for (const auto& assignment : statement.assignments) {
        auto column_index = resolve_column(*table, assignment.column);
        if (!column_index) {
            return std::unexpected(column_index.error());
        }
        for (const auto& existing : bound_statement.assignments) {
            if (existing.column_index == *column_index) {
                return std::unexpected(
                    BindError{BindErrorCode::duplicate_column,
                              assignment.column.location});
            }
        }

        auto value = bind_literal(assignment.value);
        if (!value) {
            return std::unexpected(value.error());
        }
        if (!value_matches_type(value->value,
                                table->columns[*column_index].type)) {
            return std::unexpected(
                BindError{BindErrorCode::type_mismatch, value->location});
        }
        bound_statement.assignments.push_back(
            BoundUpdateAssignment{*column_index, std::move(value->value)});
    }

    if (statement.where) {
        auto expression = bind_expression(*statement.where, table);
        if (!expression) {
            return std::unexpected(expression.error());
        }
        if (expression->result_type() !=
            BoundExpressionResultType::boolean) {
            return std::unexpected(BindError{
                BindErrorCode::type_mismatch,
                expression_location(*statement.where)});
        }
        bound_statement.where = std::move(*expression);
    }
    return bound_statement;
}

}  // namespace minidb::binder
