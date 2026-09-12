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
    if (token->type == TokenType::explain) {
        auto statement = parse_explain_statement();
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
    if (token->type == TokenType::begin) {
        auto statement = parse_begin_statement();
        if (!statement) {
            return std::unexpected(statement.error());
        }
        return Statement{*statement};
    }
    if (token->type == TokenType::commit) {
        auto statement = parse_commit_statement();
        if (!statement) {
            return std::unexpected(statement.error());
        }
        return Statement{*statement};
    }
    if (token->type == TokenType::rollback) {
        auto statement = parse_rollback_statement();
        if (!statement) {
            return std::unexpected(statement.error());
        }
        return Statement{*statement};
    }
    return std::unexpected(ParseError{ParseErrorCode::expected_statement, token->location});
}

std::expected<ExplainStatement, ParseError>
Parser::parse_explain_statement() {
    auto token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::explain) {
        return std::unexpected(ParseError{
            ParseErrorCode::expected_explain, token->location});
    }

    auto statement = parse_select_statement();
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return ExplainStatement{std::move(*statement)};
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
    token = peek_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type == TokenType::distinct) {
        statement.distinct = true;
        auto consumed = next_token();
        if (!consumed) {
            return std::unexpected(consumed.error());
        }
        token = peek_token();
        if (!token) {
            return std::unexpected(token.error());
        }
    }

    if (token->type == TokenType::star) {
        statement.select_all_columns = true;

        auto consumed = next_token();
        if (!consumed) {
            return std::unexpected(consumed.error());
        }
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
        auto table = parse_table_reference(*token);
        if (!table) {
            return std::unexpected(table.error());
        }
        statement.from = std::move(*table);

        auto tail = parse_table_statement_tail(
            statement.where, &statement.joins, &statement.group_by,
            &statement.order_by, &statement.limit, &statement.offset);
        if (!tail) {
            return std::unexpected(tail.error());
        }
        return statement;
    }

    while (true) {
        auto expression = parse_expression();
        if (!expression) {
            return std::unexpected(expression.error());
        }

        auto delimiter = next_token();
        if (!delimiter) {
            return std::unexpected(delimiter.error());
        }
        if (delimiter->type == TokenType::as_keyword) {
            auto alias = next_token();
            if (!alias) {
                return std::unexpected(alias.error());
            }
            if (alias->type != TokenType::identifier) {
                return std::unexpected(ParseError{
                    ParseErrorCode::expected_identifier,
                    alias->location});
            }
            expression->alias = ExpressionAlias{
                alias->lexeme, alias->location};
            delimiter = next_token();
            if (!delimiter) {
                return std::unexpected(delimiter.error());
            }
        }
        statement.expressions.push_back(std::move(*expression));

        if (delimiter->type == TokenType::comma) {
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
            auto table = parse_table_reference(*token);
            if (!table) {
                return std::unexpected(table.error());
            }
            statement.from = std::move(*table);

            auto tail = parse_table_statement_tail(
                statement.where, &statement.joins, &statement.group_by,
                &statement.order_by, &statement.limit, &statement.offset);
            if (!tail) {
                return std::unexpected(tail.error());
            }
            return statement;
        }
        if (delimiter->type == TokenType::group) {
            lookahead_ = *delimiter;
            auto tail = parse_table_statement_tail(
                statement.where, &statement.joins, &statement.group_by,
                &statement.order_by, &statement.limit,
                &statement.offset);
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
        if (delimiter->type == TokenType::offset) {
            auto offset = parse_offset_clause();
            if (!offset) {
                return std::unexpected(offset.error());
            }
            statement.offset = *offset;
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

std::expected<Expression, ParseError>
Parser::parse_expression() {
    return parse_disjunction_expression();
}

std::expected<Expression, ParseError>
Parser::parse_additive_expression() {
    auto expression = parse_multiplicative_expression();
    if (!expression) {
        return std::unexpected(expression.error());
    }

    auto token = peek_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    while (token->type == TokenType::plus ||
           token->type == TokenType::minus) {
        const Token operation = *token;
        auto consumed = next_token();
        if (!consumed) {
            return std::unexpected(consumed.error());
        }
        auto right = parse_multiplicative_expression();
        if (!right) {
            return std::unexpected(right.error());
        }
        expression = Expression{BinaryArithmeticExpression{
            operation.type == TokenType::plus
                ? BinaryArithmeticOperator::addition
                : BinaryArithmeticOperator::subtraction,
            std::make_unique<Expression>(std::move(*expression)),
            std::make_unique<Expression>(std::move(*right)),
            operation.location}};

        token = peek_token();
        if (!token) {
            return std::unexpected(token.error());
        }
    }
    return expression;
}

std::expected<Expression, ParseError>
Parser::parse_multiplicative_expression() {
    auto expression = parse_unary_expression();
    if (!expression) {
        return std::unexpected(expression.error());
    }

    auto token = peek_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    while (token->type == TokenType::star ||
           token->type == TokenType::slash) {
        const Token operation = *token;
        auto consumed = next_token();
        if (!consumed) {
            return std::unexpected(consumed.error());
        }
        auto right = parse_unary_expression();
        if (!right) {
            return std::unexpected(right.error());
        }
        expression = Expression{BinaryArithmeticExpression{
            operation.type == TokenType::star
                ? BinaryArithmeticOperator::multiplication
                : BinaryArithmeticOperator::division,
            std::make_unique<Expression>(std::move(*expression)),
            std::make_unique<Expression>(std::move(*right)),
            operation.location}};

        token = peek_token();
        if (!token) {
            return std::unexpected(token.error());
        }
    }
    return expression;
}

std::expected<Expression, ParseError>
Parser::parse_unary_expression() {
    auto token = peek_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::plus &&
        token->type != TokenType::minus) {
        return parse_primary_expression();
    }

    const Token operation = *token;
    auto consumed = next_token();
    if (!consumed) {
        return std::unexpected(consumed.error());
    }
    auto operand = parse_unary_expression();
    if (!operand) {
        return std::unexpected(operand.error());
    }
    return Expression{UnaryArithmeticExpression{
        operation.type == TokenType::plus
            ? UnaryArithmeticOperator::plus
            : UnaryArithmeticOperator::minus,
        std::make_unique<Expression>(std::move(*operand)),
        operation.location}};
}

std::expected<Expression, ParseError>
Parser::parse_primary_expression() {
    auto token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (is_literal(token->type)) {
        return Expression{LiteralExpression{
            literal_type(token->type), token->lexeme, token->location}};
    }
    if (token->type == TokenType::identifier) {
        auto next = peek_token();
        if (!next) {
            return std::unexpected(next.error());
        }
        if (next->type == TokenType::left_parenthesis) {
            return parse_function_call(*token);
        }
        auto column = parse_column_reference(*token);
        if (!column) {
            return std::unexpected(column.error());
        }
        return Expression{std::move(*column)};
    }
    if (token->type != TokenType::left_parenthesis) {
        return std::unexpected(ParseError{
            ParseErrorCode::expected_expression, token->location});
    }

    auto expression = parse_expression();
    if (!expression) {
        return std::unexpected(expression.error());
    }
    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::right_parenthesis) {
        return std::unexpected(ParseError{
            ParseErrorCode::expected_right_parenthesis, token->location});
    }
    return expression;
}

std::expected<Expression, ParseError>
Parser::parse_function_call(Token name) {
    auto token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }

    FunctionCallExpression call{name.lexeme, {}, false, name.location};
    token = peek_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type == TokenType::star) {
        call.star_argument = true;
        auto consumed = next_token();
        if (!consumed) {
            return std::unexpected(consumed.error());
        }
    } else {
        while (true) {
            auto argument = parse_expression();
            if (!argument) {
                return std::unexpected(argument.error());
            }
            call.arguments.push_back(
                std::make_unique<Expression>(std::move(*argument)));
            token = peek_token();
            if (!token) {
                return std::unexpected(token.error());
            }
            if (token->type != TokenType::comma) {
                break;
            }
            auto comma = next_token();
            if (!comma) {
                return std::unexpected(comma.error());
            }
        }
    }

    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::right_parenthesis) {
        return std::unexpected(ParseError{
            ParseErrorCode::expected_right_parenthesis, token->location});
    }
    return Expression{std::move(call)};
}

std::expected<ColumnReferenceExpression, ParseError>
Parser::parse_column_reference(Token first_identifier) {
    auto token = peek_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::dot) {
        return ColumnReferenceExpression{
            first_identifier.lexeme, first_identifier.location};
    }

    auto dot = next_token();
    if (!dot) {
        return std::unexpected(dot.error());
    }
    token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::identifier) {
        return std::unexpected(ParseError{
            ParseErrorCode::expected_identifier, token->location});
    }
    return ColumnReferenceExpression{
        token->lexeme, token->location,
        ColumnQualifier{first_identifier.lexeme,
                        first_identifier.location}};
}

std::expected<TableReference, ParseError>
Parser::parse_table_reference(Token name) {
    TableReference table{name.lexeme, name.location};
    auto token = peek_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type == TokenType::as_keyword) {
        auto consumed = next_token();
        if (!consumed) {
            return std::unexpected(consumed.error());
        }
        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        if (token->type != TokenType::identifier) {
            return std::unexpected(ParseError{
                ParseErrorCode::expected_identifier, token->location});
        }
        table.alias = TableAlias{token->lexeme, token->location};
        return table;
    }
    if (token->type == TokenType::identifier) {
        auto alias = next_token();
        if (!alias) {
            return std::unexpected(alias.error());
        }
        table.alias = TableAlias{alias->lexeme, alias->location};
    }
    return table;
}

std::expected<Expression, ParseError>
Parser::parse_comparison_expression() {
    auto expression = parse_additive_expression();
    if (!expression) {
        return std::unexpected(expression.error());
    }

    auto token = peek_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (is_comparison_operator(token->type)) {
        const Token operation = *token;
        auto consumed = next_token();
        if (!consumed) {
            return std::unexpected(consumed.error());
        }
        auto right = parse_additive_expression();
        if (!right) {
            return std::unexpected(right.error());
        }
        return Expression{ComparisonExpression{
            comparison_operator(operation.type),
            std::make_unique<Expression>(std::move(*expression)),
            std::make_unique<Expression>(std::move(*right)),
            operation.location}};
    }

    if (token->type != TokenType::is) {
        return expression;
    }

    const Token operation = *token;
    auto consumed = next_token();
    if (!consumed) {
        return std::unexpected(consumed.error());
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
    return Expression{NullTestExpression{
        std::make_unique<Expression>(std::move(*expression)), negated,
        operation.location}};
}

std::expected<Expression, ParseError>
Parser::parse_negation_expression() {
    auto token = peek_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::not_keyword) {
        return parse_comparison_expression();
    }

    const Token operation = *token;
    auto consumed = next_token();
    if (!consumed) {
        return std::unexpected(consumed.error());
    }
    auto operand = parse_negation_expression();
    if (!operand) {
        return std::unexpected(operand.error());
    }
    return Expression{NegationExpression{
        std::make_unique<Expression>(std::move(*operand)),
        operation.location}};
}

std::expected<Expression, ParseError>
Parser::parse_conjunction_expression() {
    auto expression = parse_negation_expression();
    if (!expression) {
        return std::unexpected(expression.error());
    }

    auto token = peek_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    while (token->type == TokenType::and_keyword) {
        const Token operation = *token;
        auto consumed = next_token();
        if (!consumed) {
            return std::unexpected(consumed.error());
        }

        auto right = parse_negation_expression();
        if (!right) {
            return std::unexpected(right.error());
        }
        expression = Expression{LogicalExpression{
            LogicalOperator::conjunction,
            std::make_unique<Expression>(std::move(*expression)),
            std::make_unique<Expression>(std::move(*right)),
            operation.location}};

        token = peek_token();
        if (!token) {
            return std::unexpected(token.error());
        }
    }
    return expression;
}

std::expected<Expression, ParseError>
Parser::parse_disjunction_expression() {
    auto expression = parse_conjunction_expression();
    if (!expression) {
        return std::unexpected(expression.error());
    }

    auto token = peek_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    while (token->type == TokenType::or_keyword) {
        const Token operation = *token;
        auto consumed = next_token();
        if (!consumed) {
            return std::unexpected(consumed.error());
        }

        auto right = parse_conjunction_expression();
        if (!right) {
            return std::unexpected(right.error());
        }
        expression = Expression{LogicalExpression{
            LogicalOperator::disjunction,
            std::make_unique<Expression>(std::move(*expression)),
            std::make_unique<Expression>(std::move(*right)),
            operation.location}};

        token = peek_token();
        if (!token) {
            return std::unexpected(token.error());
        }
    }
    return expression;
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

std::expected<OffsetClause, ParseError> Parser::parse_offset_clause() {
    auto token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::integer_literal) {
        return std::unexpected(ParseError{
            ParseErrorCode::expected_integer_literal, token->location});
    }
    return OffsetClause{token->lexeme, token->location};
}

std::expected<void, ParseError> Parser::parse_table_statement_tail(
    std::optional<Expression>& where, std::vector<JoinClause>* joins,
    std::vector<Expression>* group_by,
    std::vector<OrderByTerm>* order_by,
    std::optional<LimitClause>* limit,
    std::optional<OffsetClause>* offset) {
    auto token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }

    while (token->type == TokenType::join && joins != nullptr) {
        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        if (token->type != TokenType::identifier) {
            return std::unexpected(ParseError{
                ParseErrorCode::expected_identifier, token->location});
        }
        auto table = parse_table_reference(*token);
        if (!table) {
            return std::unexpected(table.error());
        }

        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        if (token->type != TokenType::on) {
            return std::unexpected(ParseError{
                ParseErrorCode::expected_on, token->location});
        }
        auto condition = parse_expression();
        if (!condition) {
            return std::unexpected(condition.error());
        }
        joins->push_back(JoinClause{
            std::move(*table), std::move(*condition)});

        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
    }

    if (token->type == TokenType::where) {
        auto expression = parse_expression();
        if (!expression) {
            return std::unexpected(expression.error());
        }
        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        where = std::move(*expression);
    }

    if (token->type == TokenType::group && group_by != nullptr) {
        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        if (token->type != TokenType::by) {
            return std::unexpected(
                ParseError{ParseErrorCode::expected_by, token->location});
        }

        while (true) {
            auto expression = parse_expression();
            if (!expression) {
                return std::unexpected(expression.error());
            }
            group_by->push_back(std::move(*expression));

            token = next_token();
            if (!token) {
                return std::unexpected(token.error());
            }
            if (token->type == TokenType::comma) {
                continue;
            }
            if (token->type != TokenType::order &&
                token->type != TokenType::limit &&
                token->type != TokenType::offset &&
                token->type != TokenType::semicolon &&
                token->type != TokenType::end_of_input) {
                return std::unexpected(ParseError{
                    ParseErrorCode::expected_comma_or_end,
                    token->location});
            }
            break;
        }
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

            auto column = parse_column_reference(*token);
            if (!column) {
                return std::unexpected(column.error());
            }
            OrderByTerm term{std::move(*column),
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
                token->type != TokenType::limit &&
                token->type != TokenType::offset) {
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

    if (token->type == TokenType::offset && offset != nullptr) {
        auto clause = parse_offset_clause();
        if (!clause) {
            return std::unexpected(clause.error());
        }
        *offset = *clause;
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
        auto column = parse_column_reference(*token);
        if (!column) {
            return std::unexpected(column.error());
        }

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
            std::move(*column),
            LiteralExpression{literal_type(token->type), token->lexeme,
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

std::expected<BeginStatement, ParseError> Parser::parse_begin_statement() {
    auto token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::begin) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_begin, token->location});
    }
    auto end = parse_statement_end();
    if (!end) {
        return std::unexpected(end.error());
    }
    return BeginStatement{};
}

std::expected<CommitStatement, ParseError> Parser::parse_commit_statement() {
    auto token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::commit) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_commit, token->location});
    }
    auto end = parse_statement_end();
    if (!end) {
        return std::unexpected(end.error());
    }
    return CommitStatement{};
}

std::expected<RollbackStatement, ParseError> Parser::parse_rollback_statement() {
    auto token = next_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (token->type != TokenType::rollback) {
        return std::unexpected(
            ParseError{ParseErrorCode::expected_rollback, token->location});
    }
    auto end = parse_statement_end();
    if (!end) {
        return std::unexpected(end.error());
    }
    return RollbackStatement{};
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
        token = next_token();
        if (!token) {
            return std::unexpected(token.error());
        }
        bool primary_key = false;
        if (token->type == TokenType::primary) {
            token = next_token();
            if (!token) {
                return std::unexpected(token.error());
            }
            if (token->type != TokenType::key) {
                return std::unexpected(
                    ParseError{ParseErrorCode::expected_key,
                               token->location});
            }
            primary_key = true;
            token = next_token();
            if (!token) {
                return std::unexpected(token.error());
            }
        }
        statement.columns.push_back(ColumnDefinition{
            column_name.lexeme, column_type, column_name.location,
            primary_key});

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

std::expected<void, ParseError> Parser::parse_statement_end() {
    auto token = next_token();
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
    return {};
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
