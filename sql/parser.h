#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "ast.h"
#include "lexer.h"

namespace simpledb {

class SqlParser {
public:
    explicit SqlParser(std::vector<SqlToken> tokens) : tokens_(std::move(tokens)), pos_(0) {}
    std::unique_ptr<StatementAST> ParseStatement();

private:
    struct SelectCoreAST {
        bool distinct{false};
        std::vector<SelectItemAST> select_items;
        TableRefAST from_table;
        std::optional<TableRefAST> join_table;
        std::unique_ptr<ExprAST> join_predicate;
        std::unique_ptr<ExprAST> where_predicate;
        std::vector<std::unique_ptr<ExprAST>> group_by_exprs;
    };

    const SqlToken &Peek() const;
    const SqlToken &Previous() const;
    bool Match(SqlTokenType type);
    bool MatchAny(const std::vector<SqlTokenType> &types);
    const SqlToken &Expect(SqlTokenType type, const std::string &message);

    std::unique_ptr<StatementAST> ParseCreate();
    std::unique_ptr<StatementAST> ParseInsert();
    std::unique_ptr<StatementAST> ParseSelect();
    std::unique_ptr<StatementAST> ParseUpdate();
    std::unique_ptr<StatementAST> ParseDelete();
    std::unique_ptr<StatementAST> ParseExplain();
    std::unique_ptr<CreateTableStatementAST> ParseCreateTable();
    std::unique_ptr<CreateIndexStatementAST> ParseCreateIndex();

    TableRefAST ParseTableRef();
    SelectItemAST ParseSelectItem();
    std::vector<SelectItemAST> ParseSelectList();
    std::vector<OrderByItemAST> ParseOrderByList();
    std::vector<std::unique_ptr<ExprAST>> ParseExprList();
    std::optional<std::size_t> ParseLimitClause();
    SelectCoreAST ParseSelectCore();
    bool NextTokenStartsSetOperation() const;
    SetOperationAST ParseSetOperation();

    std::vector<ColumnDefAST> ParseColumnDefs();
    ColumnDefAST ParseColumnDef();
    std::unique_ptr<ExprAST> ParseExpression();
    std::unique_ptr<ExprAST> ParseOr();
    std::unique_ptr<ExprAST> ParseAnd();
    std::unique_ptr<ExprAST> ParseComparison();
    std::unique_ptr<ExprAST> ParsePrimary();
    std::unique_ptr<ExprAST> ParseValueLiteral();
    TypeId ParseTypeName(uint32_t *out_varchar_len);

    std::vector<SqlToken> tokens_;
    std::size_t pos_;
};

} // namespace simpledb
