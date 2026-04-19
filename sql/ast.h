#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../common/types.h"
#include "../common/value.h"

namespace simpledb {

enum class SqlStatementType {
    CREATE_TABLE,
    CREATE_INDEX,
    INSERT,
    SELECT,
    COMPOUND_SELECT,
    UPDATE,
    DELETE_OP,
    EXPLAIN
};

class ExprAST {
public:
    virtual ~ExprAST() = default;
};

class LiteralExprAST : public ExprAST {
public:
    explicit LiteralExprAST(Value value) : value_(std::move(value)) {}
    const Value &GetValue() const { return value_; }
private:
    Value value_;
};

class ColumnRefExprAST : public ExprAST {
public:
    ColumnRefExprAST(std::string qualifier, std::string column_name)
        : qualifier_(std::move(qualifier)), column_name_(std::move(column_name)) {}
    const std::string &GetQualifier() const { return qualifier_; }
    const std::string &GetColumnName() const { return column_name_; }
private:
    std::string qualifier_;
    std::string column_name_;
};

class BinaryExprAST : public ExprAST {
public:
    BinaryExprAST(std::string op, std::unique_ptr<ExprAST> left, std::unique_ptr<ExprAST> right)
        : op_(std::move(op)), left_(std::move(left)), right_(std::move(right)) {}
    const std::string &GetOp() const { return op_; }
    const ExprAST *GetLeft() const { return left_.get(); }
    const ExprAST *GetRight() const { return right_.get(); }
private:
    std::string op_;
    std::unique_ptr<ExprAST> left_;
    std::unique_ptr<ExprAST> right_;
};

class FunctionCallExprAST : public ExprAST {
public:
    FunctionCallExprAST(std::string name, bool star_arg, std::unique_ptr<ExprAST> arg)
        : name_(std::move(name)), star_arg_(star_arg), arg_(std::move(arg)) {}
    const std::string &GetName() const { return name_; }
    bool IsStarArg() const { return star_arg_; }
    const ExprAST *GetArg() const { return arg_.get(); }
private:
    std::string name_;
    bool star_arg_;
    std::unique_ptr<ExprAST> arg_;
};

struct SelectItemAST {
    bool is_star{false};
    std::unique_ptr<ExprAST> expr;
    std::string alias;
};

struct OrderByItemAST {
    std::unique_ptr<ExprAST> expr;
    bool ascending{true};
};

struct TableRefAST {
    std::string table_name;
    std::string alias;
};

struct LimitClauseAST {
    std::size_t value{0};
};

enum class SetOperationType {
    UNION_OP,
    INTERSECT_OP,
    EXCEPT_OP
};

struct SetOperationAST {
    SetOperationType type{SetOperationType::UNION_OP};
    bool all{false};
};

struct ColumnDefAST {
    std::string name;
    TypeId type{TypeId::INVALID};
    bool nullable{true};
    uint32_t varchar_len{0};
};

class StatementAST {
public:
    explicit StatementAST(SqlStatementType type) : type_(type) {}
    virtual ~StatementAST() = default;
    SqlStatementType GetType() const { return type_; }
private:
    SqlStatementType type_;
};

class CreateTableStatementAST : public StatementAST {
public:
    CreateTableStatementAST(std::string table_name, std::vector<ColumnDefAST> columns)
        : StatementAST(SqlStatementType::CREATE_TABLE), table_name_(std::move(table_name)), columns_(std::move(columns)) {}
    const std::string &GetTableName() const { return table_name_; }
    const std::vector<ColumnDefAST> &GetColumns() const { return columns_; }
private:
    std::string table_name_;
    std::vector<ColumnDefAST> columns_;
};

class CreateIndexStatementAST : public StatementAST {
public:
    CreateIndexStatementAST(bool unique, std::string index_name, std::string table_name, std::vector<std::string> column_names)
        : StatementAST(SqlStatementType::CREATE_INDEX), unique_(unique), index_name_(std::move(index_name)), table_name_(std::move(table_name)), column_names_(std::move(column_names)) {}
    bool IsUnique() const { return unique_; }
    const std::string &GetIndexName() const { return index_name_; }
    const std::string &GetTableName() const { return table_name_; }
    const std::vector<std::string> &GetColumnNames() const { return column_names_; }
private:
    bool unique_;
    std::string index_name_;
    std::string table_name_;
    std::vector<std::string> column_names_;
};

class InsertStatementAST : public StatementAST {
public:
    InsertStatementAST(std::string table_name, std::vector<std::unique_ptr<ExprAST>> values)
        : StatementAST(SqlStatementType::INSERT), table_name_(std::move(table_name)), values_(std::move(values)) {}
    const std::string &GetTableName() const { return table_name_; }
    const std::vector<std::unique_ptr<ExprAST>> &GetValues() const { return values_; }
private:
    std::string table_name_;
    std::vector<std::unique_ptr<ExprAST>> values_;
};

class SelectStatementAST : public StatementAST {
public:
    SelectStatementAST(bool distinct,
                       std::vector<SelectItemAST> select_items,
                       TableRefAST from_table,
                       std::optional<TableRefAST> join_table,
                       std::unique_ptr<ExprAST> join_predicate,
                       std::unique_ptr<ExprAST> where_predicate,
                       std::vector<std::unique_ptr<ExprAST>> group_by_exprs,
                       std::vector<OrderByItemAST> order_by_items,
                       std::optional<std::size_t> limit)
        : StatementAST(SqlStatementType::SELECT),
          distinct_(distinct),
          select_items_(std::move(select_items)), from_table_(std::move(from_table)), join_table_(std::move(join_table)),
          join_predicate_(std::move(join_predicate)), where_predicate_(std::move(where_predicate)),
          group_by_exprs_(std::move(group_by_exprs)), order_by_items_(std::move(order_by_items)), limit_(limit) {}
    bool IsDistinct() const { return distinct_; }
    const std::vector<SelectItemAST> &GetSelectItems() const { return select_items_; }
    const TableRefAST &GetFromTable() const { return from_table_; }
    const std::optional<TableRefAST> &GetJoinTable() const { return join_table_; }
    const ExprAST *GetJoinPredicate() const { return join_predicate_.get(); }
    const ExprAST *GetWherePredicate() const { return where_predicate_.get(); }
    const std::vector<std::unique_ptr<ExprAST>> &GetGroupByExprs() const { return group_by_exprs_; }
    const std::vector<OrderByItemAST> &GetOrderByItems() const { return order_by_items_; }
    const std::optional<std::size_t> &GetLimit() const { return limit_; }
private:
    bool distinct_{false};
    std::vector<SelectItemAST> select_items_;
    TableRefAST from_table_;
    std::optional<TableRefAST> join_table_;
    std::unique_ptr<ExprAST> join_predicate_;
    std::unique_ptr<ExprAST> where_predicate_;
    std::vector<std::unique_ptr<ExprAST>> group_by_exprs_;
    std::vector<OrderByItemAST> order_by_items_;
    std::optional<std::size_t> limit_;
};

class CompoundSelectStatementAST : public StatementAST {
public:
    CompoundSelectStatementAST(std::vector<std::unique_ptr<SelectStatementAST>> terms,
                               std::vector<SetOperationAST> operations,
                               std::vector<OrderByItemAST> order_by_items,
                               std::optional<std::size_t> limit)
        : StatementAST(SqlStatementType::COMPOUND_SELECT),
          terms_(std::move(terms)),
          operations_(std::move(operations)),
          order_by_items_(std::move(order_by_items)),
          limit_(limit) {}
    const std::vector<std::unique_ptr<SelectStatementAST>> &GetTerms() const { return terms_; }
    const std::vector<SetOperationAST> &GetOperations() const { return operations_; }
    const std::vector<OrderByItemAST> &GetOrderByItems() const { return order_by_items_; }
    const std::optional<std::size_t> &GetLimit() const { return limit_; }
private:
    std::vector<std::unique_ptr<SelectStatementAST>> terms_;
    std::vector<SetOperationAST> operations_;
    std::vector<OrderByItemAST> order_by_items_;
    std::optional<std::size_t> limit_;
};

class UpdateStatementAST : public StatementAST {
public:
    struct Assignment {
        std::string column_name;
        std::unique_ptr<ExprAST> value_expr;
    };
    UpdateStatementAST(std::string table_name, std::vector<Assignment> assignments, std::unique_ptr<ExprAST> where_predicate)
        : StatementAST(SqlStatementType::UPDATE), table_name_(std::move(table_name)), assignments_(std::move(assignments)), where_predicate_(std::move(where_predicate)) {}
    const std::string &GetTableName() const { return table_name_; }
    const std::vector<Assignment> &GetAssignments() const { return assignments_; }
    const ExprAST *GetWherePredicate() const { return where_predicate_.get(); }
private:
    std::string table_name_;
    std::vector<Assignment> assignments_;
    std::unique_ptr<ExprAST> where_predicate_;
};

class ExplainStatementAST : public StatementAST {
public:
    explicit ExplainStatementAST(std::unique_ptr<StatementAST> inner)
        : StatementAST(SqlStatementType::EXPLAIN), inner_(std::move(inner)) {}
    const StatementAST *GetInnerStatement() const { return inner_.get(); }
private:
    std::unique_ptr<StatementAST> inner_;
};

class DeleteStatementAST : public StatementAST {
public:
    DeleteStatementAST(std::string table_name, std::unique_ptr<ExprAST> where_predicate)
        : StatementAST(SqlStatementType::DELETE_OP), table_name_(std::move(table_name)), where_predicate_(std::move(where_predicate)) {}
    const std::string &GetTableName() const { return table_name_; }
    const ExprAST *GetWherePredicate() const { return where_predicate_.get(); }
private:
    std::string table_name_;
    std::unique_ptr<ExprAST> where_predicate_;
};

} // namespace simpledb
