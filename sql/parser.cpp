#include "parser.h"

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace simpledb {

const SqlToken &SqlParser::Peek() const { return tokens_[pos_]; }
const SqlToken &SqlParser::Previous() const { return tokens_[pos_ - 1]; }

bool SqlParser::Match(SqlTokenType type) {
    if (Peek().type != type) return false;
    pos_++;
    return true;
}

bool SqlParser::MatchAny(const std::vector<SqlTokenType> &types) {
    for (SqlTokenType type : types) {
        if (Peek().type == type) {
            pos_++;
            return true;
        }
    }
    return false;
}

const SqlToken &SqlParser::Expect(SqlTokenType type, const std::string &message) {
    if (Peek().type != type) throw std::runtime_error(message);
    pos_++;
    return Previous();
}

std::unique_ptr<StatementAST> SqlParser::ParseStatement() {
    if (Match(SqlTokenType::EXPLAIN_KW)) return ParseExplain();
    if (Match(SqlTokenType::CREATE)) return ParseCreate();
    if (Match(SqlTokenType::INSERT)) return ParseInsert();
    if (Match(SqlTokenType::SELECT)) return ParseSelect();
    if (Match(SqlTokenType::UPDATE)) return ParseUpdate();
    if (Match(SqlTokenType::DELETE_KW)) return ParseDelete();
    throw std::runtime_error("Unsupported SQL statement");
}

std::unique_ptr<StatementAST> SqlParser::ParseCreate() {
    if (Peek().type == SqlTokenType::TABLE) return ParseCreateTable();
    if (Peek().type == SqlTokenType::UNIQUE || Peek().type == SqlTokenType::INDEX) return ParseCreateIndex();
    throw std::runtime_error("Expected TABLE or INDEX after CREATE");
}

std::unique_ptr<CreateTableStatementAST> SqlParser::ParseCreateTable() {
    Expect(SqlTokenType::TABLE, "Expected TABLE after CREATE");
    std::string table_name = Expect(SqlTokenType::IDENTIFIER, "Expected table name").text;
    Expect(SqlTokenType::LPAREN, "Expected '(' after table name");
    auto columns = ParseColumnDefs();
    Expect(SqlTokenType::RPAREN, "Expected ')' after column definitions");
    Match(SqlTokenType::SEMICOLON);
    return std::make_unique<CreateTableStatementAST>(table_name, std::move(columns));
}

std::unique_ptr<CreateIndexStatementAST> SqlParser::ParseCreateIndex() {
    bool unique = Match(SqlTokenType::UNIQUE);
    Expect(SqlTokenType::INDEX, "Expected INDEX");
    std::string index_name = Expect(SqlTokenType::IDENTIFIER, "Expected index name").text;
    Expect(SqlTokenType::ON, "Expected ON after index name");
    std::string table_name = Expect(SqlTokenType::IDENTIFIER, "Expected table name").text;
    Expect(SqlTokenType::LPAREN, "Expected '(' before index column list");

    std::vector<std::string> columns;
    columns.push_back(Expect(SqlTokenType::IDENTIFIER, "Expected index column").text);
    while (Match(SqlTokenType::COMMA)) {
        columns.push_back(Expect(SqlTokenType::IDENTIFIER, "Expected index column").text);
    }

    Expect(SqlTokenType::RPAREN, "Expected ')' after index columns");
    Match(SqlTokenType::SEMICOLON);
    return std::make_unique<CreateIndexStatementAST>(unique, index_name, table_name, std::move(columns));
}

std::unique_ptr<StatementAST> SqlParser::ParseInsert() {
    Expect(SqlTokenType::INTO, "Expected INTO after INSERT");
    std::string table_name = Expect(SqlTokenType::IDENTIFIER, "Expected table name").text;
    Expect(SqlTokenType::VALUES, "Expected VALUES");
    Expect(SqlTokenType::LPAREN, "Expected '(' before VALUES list");
    auto values = ParseExprList();
    Expect(SqlTokenType::RPAREN, "Expected ')' after VALUES list");
    Match(SqlTokenType::SEMICOLON);
    return std::make_unique<InsertStatementAST>(table_name, std::move(values));
}

TableRefAST SqlParser::ParseTableRef() {
    TableRefAST table;
    table.table_name = Expect(SqlTokenType::IDENTIFIER, "Expected table name").text;
    if (Match(SqlTokenType::AS)) {
        table.alias = Expect(SqlTokenType::IDENTIFIER, "Expected alias after AS").text;
    } else if (Peek().type == SqlTokenType::IDENTIFIER) {
        table.alias = Peek().text;
        pos_++;
    }
    return table;
}

SelectItemAST SqlParser::ParseSelectItem() {
    SelectItemAST item;
    if (Match(SqlTokenType::STAR)) {
        item.is_star = true;
        return item;
    }

    item.expr = ParsePrimary();
    if (Match(SqlTokenType::AS)) {
        item.alias = Expect(SqlTokenType::IDENTIFIER, "Expected alias after AS").text;
    } else if (Peek().type == SqlTokenType::IDENTIFIER) {
        item.alias = Peek().text;
        pos_++;
    }
    return item;
}

std::vector<SelectItemAST> SqlParser::ParseSelectList() {
    std::vector<SelectItemAST> items;
    items.push_back(ParseSelectItem());
    while (Match(SqlTokenType::COMMA)) items.push_back(ParseSelectItem());
    return items;
}

std::vector<OrderByItemAST> SqlParser::ParseOrderByList() {
    std::vector<OrderByItemAST> items;
    while (true) {
        OrderByItemAST item;
        item.expr = ParsePrimary();
        if (Match(SqlTokenType::ASC)) item.ascending = true;
        else if (Match(SqlTokenType::DESC)) item.ascending = false;
        items.push_back(std::move(item));
        if (!Match(SqlTokenType::COMMA)) break;
    }
    return items;
}

std::vector<std::unique_ptr<ExprAST>> SqlParser::ParseExprList() {
    std::vector<std::unique_ptr<ExprAST>> exprs;
    exprs.push_back(ParseExpression());
    while (Match(SqlTokenType::COMMA)) exprs.push_back(ParseExpression());
    return exprs;
}

std::optional<std::size_t> SqlParser::ParseLimitClause() {
    if (!Match(SqlTokenType::LIMIT_KW)) return std::nullopt;
    const SqlToken &tok = Expect(SqlTokenType::NUMBER, "Expected numeric LIMIT value");
    long long limit_value = std::stoll(tok.text);
    if (limit_value < 0) throw std::runtime_error("LIMIT cannot be negative");
    return static_cast<std::size_t>(limit_value);
}

SqlParser::SelectCoreAST SqlParser::ParseSelectCore() {
    SelectCoreAST core;
    if (Match(SqlTokenType::DISTINCT_KW)) core.distinct = true;
    else Match(SqlTokenType::ALL_KW);
    core.select_items = ParseSelectList();
    Expect(SqlTokenType::FROM, "Expected FROM");
    core.from_table = ParseTableRef();

    if (Match(SqlTokenType::JOIN)) {
        core.join_table = ParseTableRef();
        Expect(SqlTokenType::ON, "Expected ON after JOIN target");
        core.join_predicate = ParseExpression();
    }

    if (Match(SqlTokenType::WHERE)) core.where_predicate = ParseExpression();

    if (Match(SqlTokenType::GROUP)) {
        Expect(SqlTokenType::BY, "Expected BY after GROUP");
        core.group_by_exprs = ParseExprList();
    }

    return core;
}

bool SqlParser::NextTokenStartsSetOperation() const {
    SqlTokenType t = Peek().type;
    return t == SqlTokenType::UNION_KW || t == SqlTokenType::INTERSECT_KW || t == SqlTokenType::EXCEPT_KW;
}

SetOperationAST SqlParser::ParseSetOperation() {
    SetOperationAST op;
    if (Match(SqlTokenType::UNION_KW)) op.type = SetOperationType::UNION_OP;
    else if (Match(SqlTokenType::INTERSECT_KW)) op.type = SetOperationType::INTERSECT_OP;
    else if (Match(SqlTokenType::EXCEPT_KW)) op.type = SetOperationType::EXCEPT_OP;
    else throw std::runtime_error("Expected set operation");

    if (Match(SqlTokenType::ALL_KW)) op.all = true;
    else Match(SqlTokenType::DISTINCT_KW);
    return op;
}

std::unique_ptr<StatementAST> SqlParser::ParseSelect() {
    SelectCoreAST first = ParseSelectCore();

    if (!NextTokenStartsSetOperation()) {
        std::vector<OrderByItemAST> order_by;
        if (Match(SqlTokenType::ORDER)) {
            Expect(SqlTokenType::BY, "Expected BY after ORDER");
            order_by = ParseOrderByList();
        }
        std::optional<std::size_t> limit = ParseLimitClause();
        Match(SqlTokenType::SEMICOLON);
        return std::make_unique<SelectStatementAST>(
            first.distinct,
            std::move(first.select_items),
            std::move(first.from_table),
            std::move(first.join_table),
            std::move(first.join_predicate),
            std::move(first.where_predicate),
            std::move(first.group_by_exprs),
            std::move(order_by),
            limit);
    }

    std::vector<std::unique_ptr<SelectStatementAST>> terms;
    terms.push_back(std::make_unique<SelectStatementAST>(
        first.distinct,
        std::move(first.select_items),
        std::move(first.from_table),
        std::move(first.join_table),
        std::move(first.join_predicate),
        std::move(first.where_predicate),
        std::move(first.group_by_exprs),
        std::vector<OrderByItemAST>{},
        std::nullopt));

    std::vector<SetOperationAST> operations;
    while (NextTokenStartsSetOperation()) {
        operations.push_back(ParseSetOperation());
        Expect(SqlTokenType::SELECT, "Expected SELECT after set operation");
        SelectCoreAST next = ParseSelectCore();
        terms.push_back(std::make_unique<SelectStatementAST>(
            next.distinct,
            std::move(next.select_items),
            std::move(next.from_table),
            std::move(next.join_table),
            std::move(next.join_predicate),
            std::move(next.where_predicate),
            std::move(next.group_by_exprs),
            std::vector<OrderByItemAST>{},
            std::nullopt));
    }

    std::vector<OrderByItemAST> order_by;
    if (Match(SqlTokenType::ORDER)) {
        Expect(SqlTokenType::BY, "Expected BY after ORDER");
        order_by = ParseOrderByList();
    }
    std::optional<std::size_t> limit = ParseLimitClause();
    Match(SqlTokenType::SEMICOLON);
    return std::make_unique<CompoundSelectStatementAST>(std::move(terms), std::move(operations), std::move(order_by), limit);
}

std::unique_ptr<StatementAST> SqlParser::ParseUpdate() {
    std::string table_name = Expect(SqlTokenType::IDENTIFIER, "Expected table name after UPDATE").text;
    Expect(SqlTokenType::SET, "Expected SET");

    std::vector<UpdateStatementAST::Assignment> assignments;
    while (true) {
        UpdateStatementAST::Assignment assignment;
        assignment.column_name = Expect(SqlTokenType::IDENTIFIER, "Expected column name in SET clause").text;
        Expect(SqlTokenType::EQ, "Expected '=' in SET clause");
        assignment.value_expr = ParseValueLiteral();
        assignments.push_back(std::move(assignment));
        if (!Match(SqlTokenType::COMMA)) break;
    }

    std::unique_ptr<ExprAST> where;
    if (Match(SqlTokenType::WHERE)) where = ParseExpression();
    Match(SqlTokenType::SEMICOLON);
    return std::make_unique<UpdateStatementAST>(table_name, std::move(assignments), std::move(where));
}

std::unique_ptr<StatementAST> SqlParser::ParseDelete() {
    Expect(SqlTokenType::FROM, "Expected FROM after DELETE");
    std::string table_name = Expect(SqlTokenType::IDENTIFIER, "Expected table name after DELETE FROM").text;
    std::unique_ptr<ExprAST> where;
    if (Match(SqlTokenType::WHERE)) where = ParseExpression();
    Match(SqlTokenType::SEMICOLON);
    return std::make_unique<DeleteStatementAST>(table_name, std::move(where));
}

std::unique_ptr<StatementAST> SqlParser::ParseExplain() {
    auto inner = ParseStatement();
    return std::make_unique<ExplainStatementAST>(std::move(inner));
}

std::vector<ColumnDefAST> SqlParser::ParseColumnDefs() {
    std::vector<ColumnDefAST> cols;
    cols.push_back(ParseColumnDef());
    while (Match(SqlTokenType::COMMA)) cols.push_back(ParseColumnDef());
    return cols;
}

ColumnDefAST SqlParser::ParseColumnDef() {
    ColumnDefAST col;
    col.name = Expect(SqlTokenType::IDENTIFIER, "Expected column name").text;
    col.type = ParseTypeName(&col.varchar_len);
    col.nullable = true;
    if (Match(SqlTokenType::NOT)) {
        Expect(SqlTokenType::NULL_KW, "Expected NULL after NOT");
        col.nullable = false;
    }
    return col;
}

TypeId SqlParser::ParseTypeName(uint32_t *out_varchar_len) {
    *out_varchar_len = 0;
    if (Match(SqlTokenType::BOOLEAN_T)) return TypeId::BOOLEAN;
    if (MatchAny({SqlTokenType::INT_T, SqlTokenType::INTEGER_T})) return TypeId::INT32;
    if (Match(SqlTokenType::BIGINT_T)) return TypeId::INT64;
    if (Match(SqlTokenType::DOUBLE_T)) return TypeId::DOUBLE;
    if (Match(SqlTokenType::VARCHAR_T)) {
        Expect(SqlTokenType::LPAREN, "Expected '(' after VARCHAR");
        const SqlToken &len_tok = Expect(SqlTokenType::NUMBER, "Expected VARCHAR length");
        *out_varchar_len = static_cast<uint32_t>(std::stoul(len_tok.text));
        Expect(SqlTokenType::RPAREN, "Expected ')' after VARCHAR length");
        return TypeId::VARCHAR;
    }
    throw std::runtime_error("Unsupported SQL type");
}

std::unique_ptr<ExprAST> SqlParser::ParseExpression() { return ParseOr(); }

std::unique_ptr<ExprAST> SqlParser::ParseOr() {
    auto left = ParseAnd();
    while (Match(SqlTokenType::OR)) {
        auto right = ParseAnd();
        left = std::make_unique<BinaryExprAST>("OR", std::move(left), std::move(right));
    }
    return left;
}

std::unique_ptr<ExprAST> SqlParser::ParseAnd() {
    auto left = ParseComparison();
    while (Match(SqlTokenType::AND)) {
        auto right = ParseComparison();
        left = std::make_unique<BinaryExprAST>("AND", std::move(left), std::move(right));
    }
    return left;
}

std::unique_ptr<ExprAST> SqlParser::ParseComparison() {
    auto left = ParsePrimary();
    if (MatchAny({SqlTokenType::EQ, SqlTokenType::NEQ, SqlTokenType::LT, SqlTokenType::LTE, SqlTokenType::GT, SqlTokenType::GTE})) {
        std::string op = Previous().text;
        auto right = ParsePrimary();
        return std::make_unique<BinaryExprAST>(op, std::move(left), std::move(right));
    }
    return left;
}

std::unique_ptr<ExprAST> SqlParser::ParsePrimary() {
    if (Match(SqlTokenType::LPAREN)) {
        auto expr = ParseExpression();
        Expect(SqlTokenType::RPAREN, "Expected ')'");
        return expr;
    }

    if (MatchAny({SqlTokenType::COUNT, SqlTokenType::SUM, SqlTokenType::MIN, SqlTokenType::MAX})) {
        std::string func = Previous().text;
        Expect(SqlTokenType::LPAREN, "Expected '(' after aggregate function");
        bool star = false;
        std::unique_ptr<ExprAST> arg;
        if (Match(SqlTokenType::STAR)) {
            star = true;
        } else {
            arg = ParsePrimary();
        }
        Expect(SqlTokenType::RPAREN, "Expected ')' after aggregate argument");
        return std::make_unique<FunctionCallExprAST>(func, star, std::move(arg));
    }

    if (Match(SqlTokenType::IDENTIFIER)) {
        std::string first = Previous().text;
        if (Match(SqlTokenType::DOT)) {
            std::string second = Expect(SqlTokenType::IDENTIFIER, "Expected column name after '.'").text;
            return std::make_unique<ColumnRefExprAST>(first, second);
        }
        return std::make_unique<ColumnRefExprAST>("", first);
    }

    return ParseValueLiteral();
}

std::unique_ptr<ExprAST> SqlParser::ParseValueLiteral() {
    if (Match(SqlTokenType::TRUE_KW)) return std::make_unique<LiteralExprAST>(Value(true));
    if (Match(SqlTokenType::FALSE_KW)) return std::make_unique<LiteralExprAST>(Value(false));
    if (Match(SqlTokenType::NULL_KW)) return std::make_unique<LiteralExprAST>(Value::Null(TypeId::INVALID));
    if (Match(SqlTokenType::STRING)) return std::make_unique<LiteralExprAST>(Value(Previous().text));
    if (Match(SqlTokenType::NUMBER)) {
        const std::string &text = Previous().text;
        if (text.find('.') != std::string::npos) {
            return std::make_unique<LiteralExprAST>(Value(std::stod(text)));
        }
        long long v = std::stoll(text);
        if (v >= std::numeric_limits<int32_t>::min() && v <= std::numeric_limits<int32_t>::max()) {
            return std::make_unique<LiteralExprAST>(Value(static_cast<int32_t>(v)));
        }
        return std::make_unique<LiteralExprAST>(Value(static_cast<int64_t>(v)));
    }
    throw std::runtime_error("Expected literal value or identifier");
}

}  // namespace simpledb
