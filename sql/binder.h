#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "../catalog/catalog_manager.h"
#include "../execution/operators.h"
#include "../optimizer/logical_plan.h"
#include "ast.h"

namespace simpledb {

struct BoundCreateTableStatement { std::string table_name; Schema schema; };
struct BoundCreateIndexStatement {
    bool unique{false}; std::string index_name; std::string table_name; RelationId relation_id{0};
    std::vector<std::size_t> column_indexes; std::vector<IndexKeyColumnDefinition> key_columns;
};
struct BoundInsertStatement { std::string table_name; RelationId relation_id{0}; Tuple tuple; };
struct BoundUpdateStatement {
    struct Assignment { std::size_t column_idx{0}; Value value; };
    std::string table_name; RelationId relation_id{0}; std::vector<Assignment> assignments; std::unique_ptr<AbstractExpression> predicate;
};
struct BoundDeleteStatement { std::string table_name; RelationId relation_id{0}; std::unique_ptr<AbstractExpression> predicate; };
struct BoundSelectStatement {
    struct TableBinding {
        std::string table_name; std::string alias; RelationId relation_id{0}; const RelationCatalogEntry *relation{nullptr}; std::size_t offset{0};
    };
    std::vector<TableBinding> tables; Schema source_schema;
    bool has_join{false};
    std::unique_ptr<AbstractExpression> left_join_key_expr;
    std::unique_ptr<AbstractExpression> right_join_key_expr;
    std::unique_ptr<AbstractExpression> where_predicate;
    bool has_aggregate{false};
    bool distinct{false};
    bool dedup_via_group_by{false};
    bool requires_dedup{false};
    bool order_by_on_output{false};
    std::vector<std::unique_ptr<AbstractExpression>> group_by_exprs;
    std::vector<AggregateType> agg_types;
    std::vector<std::unique_ptr<AbstractExpression>> agg_input_exprs;
    Schema aggregate_schema;
    std::vector<std::unique_ptr<AbstractExpression>> project_exprs;
    Schema output_schema;
    std::vector<SortKeySpec> order_by;
    std::optional<std::size_t> limit;
    std::unique_ptr<LogicalPlanNode> logical_plan;
};
struct BoundSetOperationSpec {
    SetOperationType type{SetOperationType::UNION_OP};
    bool all{false};
};
struct BoundCompoundSelectStatement {
    std::vector<BoundSelectStatement> inputs;
    std::vector<BoundSetOperationSpec> operations;
    Schema output_schema;
    std::vector<SortKeySpec> order_by;
    std::optional<std::size_t> limit;
};
struct BoundExplainStatement {
    std::variant<BoundSelectStatement, BoundCompoundSelectStatement> query;
};

using BoundStatement = std::variant<BoundCreateTableStatement, BoundCreateIndexStatement, BoundInsertStatement, BoundSelectStatement, BoundCompoundSelectStatement, BoundUpdateStatement, BoundDeleteStatement, BoundExplainStatement>;

class SqlBinder {
public:
    explicit SqlBinder(const CatalogManager &catalog) : catalog_(catalog) {}
    BoundStatement Bind(const StatementAST &statement) const;
private:
    struct BoundExprInfo {
        std::unique_ptr<AbstractExpression> expr; TypeId type{TypeId::INVALID}; bool is_simple_column{false}; std::size_t source_column_idx{0}; std::string display_name;
    };
    const RelationCatalogEntry &ResolveRelation(const std::string &table_name) const;
    std::size_t FindColumnIndex(const Schema &schema, const std::string &column_name) const;
    Value CoerceValue(const Value &value, const Column &column) const;
    BoundCreateTableStatement BindCreateTable(const CreateTableStatementAST &stmt) const;
    BoundCreateIndexStatement BindCreateIndex(const CreateIndexStatementAST &stmt) const;
    BoundInsertStatement BindInsert(const InsertStatementAST &stmt) const;
    BoundUpdateStatement BindUpdate(const UpdateStatementAST &stmt) const;
    BoundDeleteStatement BindDelete(const DeleteStatementAST &stmt) const;
    BoundSelectStatement BindSelect(const SelectStatementAST &stmt) const;
    BoundCompoundSelectStatement BindCompoundSelect(const CompoundSelectStatementAST &stmt) const;
    BoundExprInfo BindSingleTableExpr(const ExprAST &expr_ast, const Schema &schema) const;
    BoundExprInfo BindSourceExpr(const ExprAST &expr_ast, const std::vector<BoundSelectStatement::TableBinding> &tables, const Schema &source_schema) const;
    BoundExprInfo BindOutputExpr(const ExprAST &expr_ast, const Schema &output_schema) const;
    std::unique_ptr<AbstractExpression> BindBooleanSourceExpr(const ExprAST &expr_ast, const std::vector<BoundSelectStatement::TableBinding> &tables, const Schema &source_schema) const;
    std::unique_ptr<AbstractExpression> BindBooleanSingleTableExpr(const ExprAST &expr_ast, const Schema &schema) const;
    AggregateType ParseAggregateType(const std::string &name) const;
    const CatalogManager &catalog_;
};

} // namespace simpledb
