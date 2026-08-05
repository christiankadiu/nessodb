#include "binder/binder.hpp"

#include "catalog/catalog.hpp"
#include "common/identifier.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
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

std::expected<std::size_t, BindError> bind_limit(
    const sql::LimitClause& clause) {
    std::size_t value{};
    const char* const begin = clause.count.data();
    const char* const end = begin + clause.count.size();
    const auto result = std::from_chars(begin, end, value);

    if (result.ec == std::errc::result_out_of_range) {
        return std::unexpected(
            BindError{BindErrorCode::integer_out_of_range, clause.location});
    }
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::unexpected(BindError{
            BindErrorCode::invalid_integer_literal, clause.location});
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

const sql::SourceLocation& predicate_location(
    const sql::Predicate& predicate) noexcept {
    if (const auto* comparison =
            std::get_if<sql::ComparisonPredicate>(&predicate)) {
        return comparison->column.location;
    }
    if (const auto* null_predicate =
            std::get_if<sql::NullPredicate>(&predicate)) {
        return null_predicate->column.location;
    }
    if (const auto* negation =
            std::get_if<std::unique_ptr<sql::NegationPredicate>>(&predicate)) {
        return predicate_location((*negation)->operand);
    }
    return predicate_location(
        std::get<std::unique_ptr<sql::LogicalPredicate>>(predicate)->left);
}

std::expected<BoundPredicate, BindError> bind_predicate(
    const sql::Predicate& predicate, const catalog::TableSchema& table) {
    if (const auto* comparison =
            std::get_if<sql::ComparisonPredicate>(&predicate)) {
        const std::size_t column_index =
            find_column_index(table, comparison->column.name);
        if (column_index == table.columns.size()) {
            return std::unexpected(BindError{
                BindErrorCode::column_not_found, comparison->column.location});
        }

        auto value = bind_literal(comparison->value);
        if (!value) {
            return std::unexpected(value.error());
        }
        if (!value_matches_type(value->value, table.columns[column_index].type)) {
            return std::unexpected(
                BindError{BindErrorCode::type_mismatch, value->location});
        }
        return BoundPredicate{BoundComparisonPredicate{
            column_index, bind_comparison_operator(comparison->comparison),
            std::move(value->value)}};
    }

    if (const auto* null_predicate =
            std::get_if<sql::NullPredicate>(&predicate)) {
        const std::size_t column_index =
            find_column_index(table, null_predicate->column.name);
        if (column_index == table.columns.size()) {
            return std::unexpected(BindError{
                BindErrorCode::column_not_found, null_predicate->column.location});
        }
        return BoundPredicate{
            BoundNullPredicate{column_index, null_predicate->negated}};
    }

    if (const auto* negation =
            std::get_if<std::unique_ptr<sql::NegationPredicate>>(&predicate)) {
        auto operand = bind_predicate((*negation)->operand, table);
        if (!operand) {
            return std::unexpected(operand.error());
        }
        return BoundPredicate{std::make_unique<BoundNegationPredicate>(
            BoundNegationPredicate{std::move(*operand)})};
    }

    const auto& logical =
        *std::get<std::unique_ptr<sql::LogicalPredicate>>(predicate);
    auto left = bind_predicate(logical.left, table);
    if (!left) {
        return std::unexpected(left.error());
    }
    auto right = bind_predicate(logical.right, table);
    if (!right) {
        return std::unexpected(right.error());
    }
    return BoundPredicate{std::make_unique<BoundLogicalPredicate>(
        BoundLogicalPredicate{bind_logical_operator(logical.operation),
                              std::move(*left), std::move(*right)})};
}

}  // namespace

std::expected<BoundSelectStatement, BindError> bind_select_statement(
    const sql::SelectStatement& statement, const catalog::Catalog& catalog) {
    const catalog::TableSchema* table = nullptr;
    if (statement.from) {
        table = catalog.find_table(statement.from->name);
        if (table == nullptr) {
            return std::unexpected(
                BindError{BindErrorCode::table_not_found,
                          statement.from->location});
        }
    }

    if (statement.select_all_columns) {
        if (table == nullptr) {
            return std::unexpected(BindError{BindErrorCode::table_not_found, {}});
        }
    }

    BoundSelectStatement bound_statement;
    if (table != nullptr) {
        bound_statement.table_id = table->id;
    }
    bound_statement.expressions.reserve(statement.expressions.size());

    for (const auto& expression : statement.expressions) {
        if (const auto* literal =
                std::get_if<sql::LiteralExpression>(&expression)) {
            auto bound_expression = bind_literal(*literal);
            if (!bound_expression) {
                return std::unexpected(bound_expression.error());
            }
            bound_statement.expressions.emplace_back(
                std::move(*bound_expression));
            continue;
        }

        const auto& column =
            std::get<sql::ColumnReferenceExpression>(expression);
        if (table == nullptr) {
            return std::unexpected(
                BindError{BindErrorCode::column_requires_table,
                          column.location});
        }

        const std::size_t column_index =
            find_column_index(*table, column.name);
        if (column_index == table->columns.size()) {
            return std::unexpected(
                BindError{BindErrorCode::column_not_found, column.location});
        }
        bound_statement.expressions.emplace_back(
            BoundColumnReferenceExpression{column_index, column.location});
    }

    if (statement.where) {
        if (table == nullptr) {
            return std::unexpected(
                BindError{BindErrorCode::column_requires_table,
                          predicate_location(*statement.where)});
        }
        auto predicate = bind_predicate(*statement.where, *table);
        if (!predicate) {
            return std::unexpected(predicate.error());
        }
        bound_statement.where = std::move(*predicate);
    }

    if (!statement.order_by.empty() && table == nullptr) {
        return std::unexpected(
            BindError{BindErrorCode::column_requires_table,
                      statement.order_by.front().column.location});
    }
    bound_statement.order_by.reserve(statement.order_by.size());
    for (const auto& term : statement.order_by) {
        const std::size_t column_index =
            find_column_index(*table, term.column.name);
        if (column_index == table->columns.size()) {
            return std::unexpected(
                BindError{BindErrorCode::column_not_found,
                          term.column.location});
        }
        const auto direction =
            term.direction == sql::OrderDirection::ascending
                ? BoundOrderDirection::ascending
                : BoundOrderDirection::descending;
        bound_statement.order_by.push_back(
            BoundOrderByTerm{column_index, direction});
    }

    if (statement.limit) {
        auto limit = bind_limit(*statement.limit);
        if (!limit) {
            return std::unexpected(limit.error());
        }
        bound_statement.limit = *limit;
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
        auto predicate = bind_predicate(*statement.where, *table);
        if (!predicate) {
            return std::unexpected(predicate.error());
        }
        bound_statement.where = std::move(*predicate);
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
        const std::size_t column_index =
            find_column_index(*table, assignment.column.name);
        if (column_index == table->columns.size()) {
            return std::unexpected(
                BindError{BindErrorCode::column_not_found,
                          assignment.column.location});
        }
        for (const auto& existing : bound_statement.assignments) {
            if (existing.column_index == column_index) {
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
                                table->columns[column_index].type)) {
            return std::unexpected(
                BindError{BindErrorCode::type_mismatch, value->location});
        }
        bound_statement.assignments.push_back(
            BoundUpdateAssignment{column_index, std::move(value->value)});
    }

    if (statement.where) {
        auto predicate = bind_predicate(*statement.where, *table);
        if (!predicate) {
            return std::unexpected(predicate.error());
        }
        bound_statement.where = std::move(*predicate);
    }
    return bound_statement;
}

}  // namespace minidb::binder
