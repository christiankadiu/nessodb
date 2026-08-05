#include "sql/parser.hpp"

#include <memory>
#include <utility>

namespace minidb::sql {
namespace {

bool is_literal(TokenType type) noexcept {
    return type == TokenType::integer_literal || type == TokenType::string_literal ||
           type == TokenType::null_literal;
}

LiteralType literal_type(TokenType type) noexcept {
    if (type == TokenType::integer_literal) {
        return LiteralType::integer;
    }
    if (type == TokenType::string_literal) {
        return LiteralType::string;
    }
    return LiteralType::null;
}

bool is_comparison_operator(TokenType type) noexcept {
    return type == TokenType::equal || type == TokenType::not_equal ||
           type == TokenType::less || type == TokenType::less_equal ||
           type == TokenType::greater || type == TokenType::greater_equal;
}

ComparisonOperator comparison_operator(TokenType type) noexcept {
    switch (type) {
        case TokenType::equal:
            return ComparisonOperator::equal;
        case TokenType::not_equal:
            return ComparisonOperator::not_equal;
        case TokenType::less:
            return ComparisonOperator::less;
        case TokenType::less_equal:
            return ComparisonOperator::less_equal;
        case TokenType::greater:
            return ComparisonOperator::greater;
        case TokenType::greater_equal:
            return ComparisonOperator::greater_equal;
        default:
            return ComparisonOperator::equal;
    }
}

}  // namespace

Parser::Parser(std::string_view source) noexcept : lexer_(source) {}

std::expected<Statement, ParseError> Parser::parse_statement() {
    auto token = peek_token();
    if (!token) {
        return std::unexpected(token.error());
    }

    if (token->type == TokenType::select) {
        auto statement = parse_select_statement();
        if (!statement) {
            return std::unexpected(statement.error());
        }
        return Statement{std::move(*statement)};
    }
    if (token->type == TokenType::create) {
        auto statement = parse_create_table_statement();
        if (!statement) {
            return std::unexpected(statement.error());
        }
        return Statement{std::move(*statement)};
    }
    if (token->type == TokenType::insert) {
        auto statement = parse_insert_statement();
        if (!statement) {
            return std::unexpected(statement.error());
        }
        return Statement{std::move(*statement)};
    }
    if (token->type == TokenType::delete_keyword) {
        auto statement = parse_delete_statement();
        if (!statement) {
            return std::unexpected(statement.error());
        }
        return Statement{std::move(*statement)};
    }
    if (token->type == TokenType::update) {
        auto statement = parse_update_statement();
        if (!statement) {
            return std::unexpected(statement.error());
        }
        return Statement{std::move(*statement)};
    }
    return std::unexpected(ParseError{ParseErrorCode::expected_statement, token->location});
}

std::expected<SelectStatement, ParseError> Parser::parse_select_statement() {
    auto token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::select) {
        return std::unexpected(ParseError{ParseErrorCode::expected_select, token->location});
    }

    SelectStatement statement;
    token = next_token();

    if (token && token->type == TokenType::star) {
        statement.select_all_columns = true;

        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        if (token->type != TokenType::from) {
            return std::unexpected(ParseError{ParseErrorCode::expected_from, token->location});
        }

        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        if (token->type != TokenType::identifier) {
            return std::unexpected(
                ParseError{ParseErrorCode::expected_identifier, token->location});
        }
        statement.from = TableReference{token->lexeme, token->location};

        auto tail = parse_table_statement_tail(
            statement.where, &statement.order_by, &statement.limit);
        if (!tail) {
            return std::unexpected(tail.error());
        }
        return statement;
    }

    while (true) {
        if (!token) {
            return std::unexpected(token.error());
        }
        if (is_literal(token->type)) {
            statement.expressions.emplace_back(
                LiteralExpression{literal_type(token->type), token->lexeme,
                                  token->location});
        } else if (token->type == TokenType::identifier) {
            statement.expressions.emplace_back(
                ColumnReferenceExpression{token->lexeme, token->location});
        } else {
            return std::unexpected(
                ParseError{ParseErrorCode::expected_expression, token->location});
        }

        auto delimiter = next_token();
        if (!delimiter) {
            return std::unexpected(delimiter.error());
        }

        if (delimiter->type == TokenType::comma) {
            token = next_token();
            continue;
        }
        if (delimiter->type == TokenType::from) {
            token = next_token();
            if (!token) {
                return std::unexpected(token.error());
            }
            if (token->type != TokenType::identifier) {
                return std::unexpected(
                    ParseError{ParseErrorCode::expected_identifier,
                               token->location});
            }
            statement.from = TableReference{token->lexeme, token->location};

            auto tail = parse_table_statement_tail(
                statement.where, &statement.order_by, &statement.limit);
            if (!tail) {
                return std::unexpected(tail.error());
            }
            return statement;
        }
        if (delimiter->type == TokenType::limit) {
            auto limit = parse_limit_clause();
            if (!limit) {
                return std::unexpected(limit.error());
            }
            statement.limit = *limit;
            delimiter = next_token();
            if (!delimiter) {
                return std::unexpected(delimiter.error());
            }
        }
        if (delimiter->type == TokenType::end_of_input) {
            return statement;
        }
        if (delimiter->type != TokenType::semicolon) {
            return std::unexpected(
                ParseError{ParseErrorCode::expected_comma_from_or_end,
                           delimiter->location});
        }

        auto end = next_token();
        if (!end) {
            return std::unexpected(end.error());
        }
        if (end->type != TokenType::end_of_input) {
            return std::unexpected(
                ParseError{ParseErrorCode::expected_end_of_input, end->location});
        }
        return statement;
    }
}

std::expected<Predicate, ParseError> Parser::parse_primary_predicate() {
    auto token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }

    if (token->type == TokenType::not_keyword) {
        auto operand = parse_primary_predicate();
        if (!operand) {
            return std::unexpected(operand.error());
        }
        return Predicate{std::make_unique<NegationPredicate>(
            NegationPredicate{std::move(*operand)})};
    }

    if (token->type == TokenType::left_parenthesis) {
        auto predicate = parse_predicate();
        if (!predicate) {
            return std::unexpected(predicate.error());
        }

        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        if (token->type != TokenType::right_parenthesis) {
            return std::unexpected(ParseError{
                ParseErrorCode::expected_right_parenthesis, token->location});
        }
        return predicate;
    }

    if (token->type != TokenType::identifier) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_identifier, token->location});
    }
    const ColumnReferenceExpression column{token->lexeme, token->location};

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }

    if (is_comparison_operator(token->type)) {
        const ComparisonOperator comparison = comparison_operator(token->type);

        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        if (!is_literal(token->type)) {
            return std::unexpected(
                ParseError{ParseErrorCode::expected_literal, token->location});
        }
        return Predicate{ComparisonPredicate{
            column, comparison,
            LiteralExpression{literal_type(token->type), token->lexeme,
                              token->location}}};
    }

    if (token->type != TokenType::is) {
        return std::unexpected(ParseError{
            ParseErrorCode::expected_predicate_operator, token->location});
    }

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }

    bool negated = false;
    if (token->type == TokenType::not_keyword) {
        negated = true;
        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
    }
    if (token->type != TokenType::null_literal) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_null, token->location});
    }
    return Predicate{NullPredicate{column, negated}};
}

std::expected<Predicate, ParseError> Parser::parse_conjunction() {
    auto predicate = parse_primary_predicate();
    if (!predicate) {
        return std::unexpected(predicate.error());
    }

    auto token = peek_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    while (token->type == TokenType::and_keyword) {
        auto consumed = next_token();
        if (!consumed) {
            return std::unexpected(consumed.error());
        }

        auto right = parse_primary_predicate();
        if (!right) {
            return std::unexpected(right.error());
        }
        predicate = Predicate{std::make_unique<LogicalPredicate>(
            LogicalPredicate{LogicalOperator::conjunction,
                             std::move(*predicate), std::move(*right)})};

        token = peek_token();
        if (!token) {
            return std::unexpected(token.error());
        }
    }
    return predicate;
}

std::expected<Predicate, ParseError> Parser::parse_predicate() {
    auto predicate = parse_conjunction();
    if (!predicate) {
        return std::unexpected(predicate.error());
    }

    auto token = peek_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    while (token->type == TokenType::or_keyword) {
        auto consumed = next_token();
        if (!consumed) {
            return std::unexpected(consumed.error());
        }

        auto right = parse_conjunction();
        if (!right) {
            return std::unexpected(right.error());
        }
        predicate = Predicate{std::make_unique<LogicalPredicate>(
            LogicalPredicate{LogicalOperator::disjunction,
                             std::move(*predicate), std::move(*right)})};

        token = peek_token();
        if (!token) {
            return std::unexpected(token.error());
        }
    }
    return predicate;
}

std::expected<LimitClause, ParseError> Parser::parse_limit_clause() {
    auto token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::integer_literal) {
        return std::unexpected(ParseError{
            ParseErrorCode::expected_integer_literal, token->location});
    }
    return LimitClause{token->lexeme, token->location};
}

std::expected<void, ParseError> Parser::parse_table_statement_tail(
    std::optional<Predicate>& where, std::vector<OrderByTerm>* order_by,
    std::optional<LimitClause>* limit) {
    auto token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }

    if (token->type == TokenType::where) {
        auto predicate = parse_predicate();
        if (!predicate) {
            return std::unexpected(predicate.error());
        }
        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        where = std::move(*predicate);
    }

    if (token->type == TokenType::order && order_by != nullptr) {
        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        if (token->type != TokenType::by) {
            return std::unexpected(
                ParseError{ParseErrorCode::expected_by, token->location});
        }

        token = next_token();
        while (true) {
            if (!token) {
                return std::unexpected(token.error());
            }
            if (token->type != TokenType::identifier) {
                return std::unexpected(ParseError{
                    ParseErrorCode::expected_identifier, token->location});
            }

            OrderByTerm term{
                ColumnReferenceExpression{token->lexeme, token->location},
                OrderDirection::ascending};
            token = next_token();
            if (!token) {
                return std::unexpected(token.error());
            }
            if (token->type == TokenType::asc ||
                token->type == TokenType::desc) {
                term.direction = token->type == TokenType::asc
                                     ? OrderDirection::ascending
                                     : OrderDirection::descending;
                token = next_token();
                if (!token) {
                    return std::unexpected(token.error());
                }
            }
            order_by->push_back(term);

            if (token->type == TokenType::comma) {
                token = next_token();
                continue;
            }
            if (token->type != TokenType::semicolon &&
                token->type != TokenType::end_of_input &&
                token->type != TokenType::limit) {
                return std::unexpected(ParseError{
                    ParseErrorCode::expected_comma_or_end,
                    token->location});
            }
            break;
        }
    }

    if (token->type == TokenType::limit && limit != nullptr) {
        auto clause = parse_limit_clause();
        if (!clause) {
            return std::unexpected(clause.error());
        }
        *limit = *clause;
        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
    }

    if (token->type == TokenType::semicolon) {
        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
    }
    if (token->type != TokenType::end_of_input) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_end_of_input, token->location});
    }
    return {};
}

std::expected<DeleteStatement, ParseError> Parser::parse_delete_statement() {
    auto token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::delete_keyword) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_delete, token->location});
    }

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::from) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_from, token->location});
    }

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::identifier) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_identifier, token->location});
    }

    DeleteStatement statement{TableReference{token->lexeme, token->location},
                              std::nullopt};
    auto tail = parse_table_statement_tail(statement.where);
    if (!tail) {
        return std::unexpected(tail.error());
    }
    return statement;
}

std::expected<UpdateStatement, ParseError> Parser::parse_update_statement() {
    auto token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::update) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_update, token->location});
    }

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::identifier) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_identifier, token->location});
    }
    UpdateStatement statement{TableReference{token->lexeme, token->location},
                              {}, std::nullopt};

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::set) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_set, token->location});
    }

    while (true) {
        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        if (token->type != TokenType::identifier) {
            return std::unexpected(
                ParseError{ParseErrorCode::expected_identifier,
                           token->location});
        }
        const ColumnReferenceExpression column{token->lexeme,
                                               token->location};

        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        if (token->type != TokenType::equal) {
            return std::unexpected(
                ParseError{ParseErrorCode::expected_equal, token->location});
        }

        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        if (!is_literal(token->type)) {
            return std::unexpected(
                ParseError{ParseErrorCode::expected_literal, token->location});
        }
        statement.assignments.push_back(UpdateAssignment{
            column, LiteralExpression{literal_type(token->type), token->lexeme,
                                      token->location}});

        token = peek_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        if (token->type == TokenType::comma) {
            auto comma = next_token();
            if (!comma) {
                return std::unexpected(comma.error());
            }
            continue;
        }
        if (token->type != TokenType::where &&
            token->type != TokenType::semicolon &&
            token->type != TokenType::end_of_input) {
            return std::unexpected(ParseError{
                ParseErrorCode::expected_comma_where_or_end,
                token->location});
        }
        break;
    }

    auto tail = parse_table_statement_tail(statement.where);
    if (!tail) {
        return std::unexpected(tail.error());
    }
    return statement;
}

std::expected<CreateTableStatement, ParseError> Parser::parse_create_table_statement() {
    auto token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::create) {
        return std::unexpected(ParseError{ParseErrorCode::expected_create, token->location});
    }

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::table) {
        return std::unexpected(ParseError{ParseErrorCode::expected_table, token->location});
    }

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::identifier) {
        return std::unexpected(ParseError{ParseErrorCode::expected_identifier, token->location});
    }

    CreateTableStatement statement{token->lexeme, token->location, {}};

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::left_parenthesis) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_left_parenthesis, token->location});
    }

    token = next_token();
    while (true) {
        if (!token) {
            return std::unexpected(token.error());
        }
        if (token->type != TokenType::identifier) {
            return std::unexpected(
                ParseError{ParseErrorCode::expected_identifier, token->location});
        }

        const Token column_name = *token;
        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }

        ColumnType column_type;
        if (token->type == TokenType::integer_type) {
            column_type = ColumnType::integer;
        } else if (token->type == TokenType::text_type) {
            column_type = ColumnType::text;
        } else {
            return std::unexpected(
                ParseError{ParseErrorCode::expected_column_type, token->location});
        }
        statement.columns.push_back(
            ColumnDefinition{column_name.lexeme, column_type, column_name.location});

        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        if (token->type == TokenType::right_parenthesis) {
            break;
        }
        if (token->type != TokenType::comma) {
            return std::unexpected(ParseError{
                ParseErrorCode::expected_comma_or_right_parenthesis, token->location});
        }
        token = next_token();
    }

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type == TokenType::semicolon) {
        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
    }
    if (token->type != TokenType::end_of_input) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_end_of_input, token->location});
    }

    return statement;
}

std::expected<InsertStatement, ParseError> Parser::parse_insert_statement() {
    auto token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::insert) {
        return std::unexpected(ParseError{ParseErrorCode::expected_insert, token->location});
    }

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::into) {
        return std::unexpected(ParseError{ParseErrorCode::expected_into, token->location});
    }

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::identifier) {
        return std::unexpected(ParseError{ParseErrorCode::expected_identifier, token->location});
    }
    InsertStatement statement{token->lexeme, token->location, {}};

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::values) {
        return std::unexpected(ParseError{ParseErrorCode::expected_values, token->location});
    }

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::left_parenthesis) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_left_parenthesis, token->location});
    }

    token = next_token();
    while (true) {
        if (!token) {
            return std::unexpected(token.error());
        }
        if (!is_literal(token->type)) {
            return std::unexpected(ParseError{ParseErrorCode::expected_literal, token->location});
        }
        statement.values.push_back(
            LiteralExpression{literal_type(token->type), token->lexeme, token->location});

        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        if (token->type == TokenType::right_parenthesis) {
            break;
        }
        if (token->type != TokenType::comma) {
            return std::unexpected(ParseError{
                ParseErrorCode::expected_comma_or_right_parenthesis, token->location});
        }
        token = next_token();
    }

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type == TokenType::semicolon) {
        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
    }
    if (token->type != TokenType::end_of_input) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_end_of_input, token->location});
    }
    return statement;
}

std::expected<Token, ParseError> Parser::next_token() {
    if (lookahead_) {
        Token token = *lookahead_;
        lookahead_.reset();
        return token;
    }

    auto token = lexer_.next();
    if (!token) {
        return std::unexpected(
            ParseError{ParseErrorCode::lexical_error, token.error().location});
    }
    return *token;
}

std::expected<Token, ParseError> Parser::peek_token() {
    if (!lookahead_) {
        auto token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        lookahead_ = *token;
    }
    return *lookahead_;
}

}  // namespace minidb::sql
