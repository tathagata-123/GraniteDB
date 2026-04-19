#include "binder.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace simpledb {
namespace {

Schema BuildSchemaFromColumnDefs(const std::vector<ColumnDefAST> &defs) {
    std::vector<Column> cols;
    cols.reserve(defs.size());
    for (const auto &d : defs) {
        cols.emplace_back(d.name, d.type, d.nullable, d.varchar_len);
    }
    return Schema(std::move(cols));
}

Schema BuildJoinedSchema(const Schema &left, const Schema &right) {
    std::vector<Column> cols = left.GetColumns();
    for (const auto &col : right.GetColumns()) {
        cols.push_back(col);
    }
    return Schema(std::move(cols));
}

std::string DefaultSelectItemName(const ExprAST &expr_ast) {
    if (auto c = dynamic_cast<const ColumnRefExprAST *>(&expr_ast)) return c->GetColumnName();
    if (auto f = dynamic_cast<const FunctionCallExprAST *>(&expr_ast)) return f->GetName();
    return "expr";
}

std::string UpperCopy(const std::string &s) {
    std::string out = s;
    for (char &c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

TypeId InferAggregateOutputType(AggregateType agg_type, TypeId input_type, bool star_arg) {
    switch (agg_type) {
        case AggregateType::COUNT:
            return TypeId::INT64;
        case AggregateType::SUM:
            if (star_arg) throw std::runtime_error("SUM(*) is not supported");
            if (!IsNumericType(input_type)) throw std::runtime_error("SUM requires numeric input");
            return input_type == TypeId::DOUBLE ? TypeId::DOUBLE : TypeId::INT64;
        case AggregateType::MIN:
        case AggregateType::MAX:
            if (star_arg) throw std::runtime_error("MIN/MAX(*) are not supported");
            return input_type;
    }
    throw std::runtime_error("Unknown aggregate type");
}

}  // namespace

BoundStatement SqlBinder::Bind(const StatementAST &statement) const {
    switch (statement.GetType()) {
        case SqlStatementType::CREATE_TABLE:
            return BindCreateTable(dynamic_cast<const CreateTableStatementAST &>(statement));
        case SqlStatementType::CREATE_INDEX:
            return BindCreateIndex(dynamic_cast<const CreateIndexStatementAST &>(statement));
        case SqlStatementType::INSERT:
            return BindInsert(dynamic_cast<const InsertStatementAST &>(statement));
        case SqlStatementType::SELECT:
            return BindSelect(dynamic_cast<const SelectStatementAST &>(statement));
        case SqlStatementType::COMPOUND_SELECT:
            return BindCompoundSelect(dynamic_cast<const CompoundSelectStatementAST &>(statement));
        case SqlStatementType::UPDATE:
            return BindUpdate(dynamic_cast<const UpdateStatementAST &>(statement));
        case SqlStatementType::DELETE_OP:
            return BindDelete(dynamic_cast<const DeleteStatementAST &>(statement));
        case SqlStatementType::EXPLAIN: {
            const auto &explain = dynamic_cast<const ExplainStatementAST &>(statement);
            BoundStatement inner = Bind(*explain.GetInnerStatement());
            if (std::holds_alternative<BoundSelectStatement>(inner)) {
                return BoundExplainStatement{std::variant<BoundSelectStatement, BoundCompoundSelectStatement>(std::move(std::get<BoundSelectStatement>(inner)))};
            }
            if (std::holds_alternative<BoundCompoundSelectStatement>(inner)) {
                return BoundExplainStatement{std::variant<BoundSelectStatement, BoundCompoundSelectStatement>(std::move(std::get<BoundCompoundSelectStatement>(inner)))};
            }
            throw std::runtime_error("EXPLAIN currently supports only SELECT statements");
        }
    }
    throw std::runtime_error("Unsupported statement type during bind");
}

const RelationCatalogEntry &SqlBinder::ResolveRelation(const std::string &table_name) const {
    return catalog_.GetRelationByName(table_name);
}

std::size_t SqlBinder::FindColumnIndex(const Schema &schema, const std::string &column_name) const {
    for (std::size_t i = 0; i < schema.GetColumnCount(); i++) {
        if (schema.GetColumn(i).GetName() == column_name) return i;
    }
    throw std::runtime_error("Unknown column: " + column_name);
}

Value SqlBinder::CoerceValue(const Value &value, const Column &column) const {
    if (value.IsNull()) {
        if (!column.IsNullable()) {
            throw std::runtime_error("NULL assigned to non-nullable column: " + column.GetName());
        }
        return Value::Null(column.GetType());
    }

    if (value.GetTypeId() == column.GetType()) return value;

    if (column.GetType() == TypeId::INT64 && value.GetTypeId() == TypeId::INT32) {
        return Value(static_cast<int64_t>(value.AsInt32()));
    }

    if (column.GetType() == TypeId::DOUBLE &&
        (value.GetTypeId() == TypeId::INT32 || value.GetTypeId() == TypeId::INT64)) {
        if (value.GetTypeId() == TypeId::INT32) return Value(static_cast<double>(value.AsInt32()));
        return Value(static_cast<double>(value.AsInt64()));
    }

    if (column.GetType() == TypeId::INT32 && value.GetTypeId() == TypeId::INT64) {
        int64_t v = value.AsInt64();
        if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max()) {
            throw std::runtime_error("INT64 literal out of INT32 range for column: " + column.GetName());
        }
        return Value(static_cast<int32_t>(v));
    }

    throw std::runtime_error("Type mismatch for column: " + column.GetName());
}

BoundCreateTableStatement SqlBinder::BindCreateTable(const CreateTableStatementAST &stmt) const {
    return BoundCreateTableStatement{stmt.GetTableName(), BuildSchemaFromColumnDefs(stmt.GetColumns())};
}

BoundCreateIndexStatement SqlBinder::BindCreateIndex(const CreateIndexStatementAST &stmt) const {
    BoundCreateIndexStatement bound;
    bound.unique = stmt.IsUnique();
    bound.index_name = stmt.GetIndexName();
    bound.table_name = stmt.GetTableName();

    const RelationCatalogEntry &rel = ResolveRelation(stmt.GetTableName());
    bound.relation_id = rel.relation_id;
    for (const std::string &col_name : stmt.GetColumnNames()) {
        std::size_t idx = FindColumnIndex(rel.schema, col_name);
        const Column &col = rel.schema.GetColumn(idx);
        bound.column_indexes.push_back(idx);
        bound.key_columns.push_back(IndexKeyColumnDefinition{idx, col.GetType(), col.GetMaxLength(), col.IsNullable()});
    }
    return bound;
}

SqlBinder::BoundExprInfo SqlBinder::BindSingleTableExpr(const ExprAST &expr_ast, const Schema &schema) const {
    if (auto lit = dynamic_cast<const LiteralExprAST *>(&expr_ast)) {
        return {std::make_unique<ConstantValueExpression>(lit->GetValue()),
                lit->GetValue().GetTypeId(),
                false,
                0,
                lit->GetValue().ToString()};
    }

    if (auto col = dynamic_cast<const ColumnRefExprAST *>(&expr_ast)) {
        if (!col->GetQualifier().empty()) {
            throw std::runtime_error("Qualified column references are not allowed here");
        }
        std::size_t idx = FindColumnIndex(schema, col->GetColumnName());
        BoundExprInfo out;
        out.expr = std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, idx);
        out.type = schema.GetColumn(idx).GetType();
        out.is_simple_column = true;
        out.source_column_idx = idx;
        out.display_name = schema.GetColumn(idx).GetName();
        return out;
    }

    throw std::runtime_error("Only literals and columns are supported in this SQL position");
}

SqlBinder::BoundExprInfo SqlBinder::BindSourceExpr(const ExprAST &expr_ast,
                                                   const std::vector<BoundSelectStatement::TableBinding> &tables,
                                                   const Schema &source_schema) const {
    if (auto lit = dynamic_cast<const LiteralExprAST *>(&expr_ast)) {
        return {std::make_unique<ConstantValueExpression>(lit->GetValue()),
                lit->GetValue().GetTypeId(),
                false,
                0,
                lit->GetValue().ToString()};
    }

    if (auto col = dynamic_cast<const ColumnRefExprAST *>(&expr_ast)) {
        const BoundSelectStatement::TableBinding *matched_table = nullptr;
        std::size_t matched_col_idx = 0;
        bool found = false;

        for (const auto &table : tables) {
            if (!col->GetQualifier().empty() && col->GetQualifier() != table.alias &&
                col->GetQualifier() != table.table_name) {
                continue;
            }
            for (std::size_t i = 0; i < table.relation->schema.GetColumnCount(); i++) {
                if (table.relation->schema.GetColumn(i).GetName() == col->GetColumnName()) {
                    if (found) {
                        throw std::runtime_error("Ambiguous column reference: " + col->GetColumnName());
                    }
                    found = true;
                    matched_table = &table;
                    matched_col_idx = i;
                }
            }
        }

        if (!found || matched_table == nullptr) {
            throw std::runtime_error("Unknown column reference: " + col->GetColumnName());
        }

        std::size_t absolute_idx = matched_table->offset + matched_col_idx;
        BoundExprInfo out;
        out.expr = std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, absolute_idx);
        out.type = source_schema.GetColumn(absolute_idx).GetType();
        out.is_simple_column = true;
        out.source_column_idx = absolute_idx;
        out.display_name = source_schema.GetColumn(absolute_idx).GetName();
        return out;
    }

    if (auto bin = dynamic_cast<const BinaryExprAST *>(&expr_ast)) {
        const std::string op = UpperCopy(bin->GetOp());
        if (op == "AND" || op == "OR") {
            BoundExprInfo left = BindSourceExpr(*bin->GetLeft(), tables, source_schema);
            BoundExprInfo right = BindSourceExpr(*bin->GetRight(), tables, source_schema);
            BoundExprInfo out;
            if (op == "AND") {
                out.expr = std::make_unique<ConjunctionExpression>(std::move(left.expr), std::move(right.expr));
            } else {
                out.expr = std::make_unique<DisjunctionExpression>(std::move(left.expr), std::move(right.expr));
            }
            out.type = TypeId::BOOLEAN;
            out.display_name = "bool_expr";
            return out;
        }

        BoundExprInfo left = BindSourceExpr(*bin->GetLeft(), tables, source_schema);
        BoundExprInfo right = BindSourceExpr(*bin->GetRight(), tables, source_schema);
        ComparisonType cmp;
        if (op == "=") cmp = ComparisonType::EQ;
        else if (op == "!=") cmp = ComparisonType::NEQ;
        else if (op == "<") cmp = ComparisonType::LT;
        else if (op == "<=") cmp = ComparisonType::LTE;
        else if (op == ">") cmp = ComparisonType::GT;
        else if (op == ">=") cmp = ComparisonType::GTE;
        else throw std::runtime_error("Unsupported operator in source expression: " + op);

        BoundExprInfo out;
        out.expr = std::make_unique<ComparisonExpression>(cmp, std::move(left.expr), std::move(right.expr));
        out.type = TypeId::BOOLEAN;
        out.display_name = "cmp_expr";
        return out;
    }

    throw std::runtime_error("Unsupported expression in source binding");
}

SqlBinder::BoundExprInfo SqlBinder::BindOutputExpr(const ExprAST &expr_ast, const Schema &output_schema) const {
    if (auto col = dynamic_cast<const ColumnRefExprAST *>(&expr_ast)) {
        if (!col->GetQualifier().empty()) {
            throw std::runtime_error("ORDER BY only supports output columns or aliases in this frontend");
        }
        std::size_t idx = FindColumnIndex(output_schema, col->GetColumnName());
        BoundExprInfo out;
        out.expr = std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, idx);
        out.type = output_schema.GetColumn(idx).GetType();
        out.is_simple_column = true;
        out.source_column_idx = idx;
        out.display_name = output_schema.GetColumn(idx).GetName();
        return out;
    }
    throw std::runtime_error("ORDER BY only supports output columns or aliases in this frontend");
}

std::unique_ptr<AbstractExpression> SqlBinder::BindBooleanSourceExpr(
    const ExprAST &expr_ast,
    const std::vector<BoundSelectStatement::TableBinding> &tables,
    const Schema &source_schema) const {
    BoundExprInfo bound = BindSourceExpr(expr_ast, tables, source_schema);
    return std::move(bound.expr);
}

std::unique_ptr<AbstractExpression> SqlBinder::BindBooleanSingleTableExpr(const ExprAST &expr_ast,
                                                                          const Schema &schema) const {
    if (auto lit = dynamic_cast<const LiteralExprAST *>(&expr_ast)) {
        return std::make_unique<ConstantValueExpression>(lit->GetValue());
    }

    if (auto col = dynamic_cast<const ColumnRefExprAST *>(&expr_ast)) {
        if (!col->GetQualifier().empty()) {
            throw std::runtime_error("Qualified columns are not supported in single-table DML predicates");
        }
        std::size_t idx = FindColumnIndex(schema, col->GetColumnName());
        return std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, idx);
    }

    if (auto bin = dynamic_cast<const BinaryExprAST *>(&expr_ast)) {
        std::string op = UpperCopy(bin->GetOp());
        if (op == "AND" || op == "OR") {
            auto left = BindBooleanSingleTableExpr(*bin->GetLeft(), schema);
            auto right = BindBooleanSingleTableExpr(*bin->GetRight(), schema);
            if (op == "AND") return std::make_unique<ConjunctionExpression>(std::move(left), std::move(right));
            return std::make_unique<DisjunctionExpression>(std::move(left), std::move(right));
        }

        BoundExprInfo left = BindSingleTableExpr(*bin->GetLeft(), schema);
        BoundExprInfo right = BindSingleTableExpr(*bin->GetRight(), schema);
        ComparisonType cmp;
        if (op == "=") cmp = ComparisonType::EQ;
        else if (op == "!=") cmp = ComparisonType::NEQ;
        else if (op == "<") cmp = ComparisonType::LT;
        else if (op == "<=") cmp = ComparisonType::LTE;
        else if (op == ">") cmp = ComparisonType::GT;
        else if (op == ">=") cmp = ComparisonType::GTE;
        else throw std::runtime_error("Unsupported DML predicate operator: " + op);
        return std::make_unique<ComparisonExpression>(cmp, std::move(left.expr), std::move(right.expr));
    }

    throw std::runtime_error("Unsupported DML predicate expression");
}

AggregateType SqlBinder::ParseAggregateType(const std::string &name) const {
    std::string n = UpperCopy(name);
    if (n == "COUNT") return AggregateType::COUNT;
    if (n == "SUM") return AggregateType::SUM;
    if (n == "MIN") return AggregateType::MIN;
    if (n == "MAX") return AggregateType::MAX;
    throw std::runtime_error("Unsupported aggregate: " + name);
}

BoundInsertStatement SqlBinder::BindInsert(const InsertStatementAST &stmt) const {
    const RelationCatalogEntry &rel = ResolveRelation(stmt.GetTableName());
    if (stmt.GetValues().size() != rel.schema.GetColumnCount()) {
        throw std::runtime_error("INSERT value count does not match table schema");
    }

    std::vector<Value> values;
    values.reserve(stmt.GetValues().size());
    for (std::size_t i = 0; i < stmt.GetValues().size(); i++) {
        auto *lit = dynamic_cast<const LiteralExprAST *>(stmt.GetValues()[i].get());
        if (lit == nullptr) throw std::runtime_error("INSERT currently accepts only literal VALUES");
        values.push_back(CoerceValue(lit->GetValue(), rel.schema.GetColumn(i)));
    }

    return BoundInsertStatement{stmt.GetTableName(), rel.relation_id, Tuple(std::move(values))};
}

BoundUpdateStatement SqlBinder::BindUpdate(const UpdateStatementAST &stmt) const {
    const RelationCatalogEntry &rel = ResolveRelation(stmt.GetTableName());
    BoundUpdateStatement bound;
    bound.table_name = stmt.GetTableName();
    bound.relation_id = rel.relation_id;

    for (const auto &assignment : stmt.GetAssignments()) {
        auto *lit = dynamic_cast<const LiteralExprAST *>(assignment.value_expr.get());
        if (lit == nullptr) throw std::runtime_error("UPDATE SET currently accepts only literal right-hand sides");
        std::size_t idx = FindColumnIndex(rel.schema, assignment.column_name);
        bound.assignments.push_back({idx, CoerceValue(lit->GetValue(), rel.schema.GetColumn(idx))});
    }

    if (stmt.GetWherePredicate() != nullptr) {
        bound.predicate = BindBooleanSingleTableExpr(*stmt.GetWherePredicate(), rel.schema);
    }
    return bound;
}

BoundDeleteStatement SqlBinder::BindDelete(const DeleteStatementAST &stmt) const {
    const RelationCatalogEntry &rel = ResolveRelation(stmt.GetTableName());
    BoundDeleteStatement bound;
    bound.table_name = stmt.GetTableName();
    bound.relation_id = rel.relation_id;
    if (stmt.GetWherePredicate() != nullptr) {
        bound.predicate = BindBooleanSingleTableExpr(*stmt.GetWherePredicate(), rel.schema);
    }
    return bound;
}

BoundSelectStatement SqlBinder::BindSelect(const SelectStatementAST &stmt) const {
    BoundSelectStatement bound;
    bound.limit = stmt.GetLimit();

    const RelationCatalogEntry &left_rel = ResolveRelation(stmt.GetFromTable().table_name);
    BoundSelectStatement::TableBinding left_table;
    left_table.table_name = stmt.GetFromTable().table_name;
    left_table.alias = stmt.GetFromTable().alias.empty() ? stmt.GetFromTable().table_name : stmt.GetFromTable().alias;
    left_table.relation_id = left_rel.relation_id;
    left_table.relation = &left_rel;
    left_table.offset = 0;
    bound.tables.push_back(left_table);
    bound.source_schema = left_rel.schema;

    std::unique_ptr<LogicalPlanNode> left_logical =
        std::make_unique<LogicalGetNode>(left_rel.relation_id, left_rel.schema);
    std::unique_ptr<LogicalPlanNode> logical_root;

    std::optional<BoundSelectStatement::TableBinding> right_table_opt;
    const RelationCatalogEntry *right_rel_ptr = nullptr;
    std::unique_ptr<LogicalPlanNode> right_logical;

    if (stmt.GetJoinTable().has_value()) {
        const RelationCatalogEntry &right_rel = ResolveRelation(stmt.GetJoinTable()->table_name);
        BoundSelectStatement::TableBinding right_table;
        right_table.table_name = stmt.GetJoinTable()->table_name;
        right_table.alias = stmt.GetJoinTable()->alias.empty() ? stmt.GetJoinTable()->table_name : stmt.GetJoinTable()->alias;
        right_table.relation_id = right_rel.relation_id;
        right_table.relation = &right_rel;
        right_table.offset = bound.source_schema.GetColumnCount();
        if (left_table.alias == right_table.alias) {
            throw std::runtime_error("Duplicate table alias in SELECT");
        }
        bound.tables.push_back(right_table);
        right_table_opt = right_table;
        right_rel_ptr = &right_rel;
        right_logical = std::make_unique<LogicalGetNode>(right_rel.relation_id, right_rel.schema);
        bound.source_schema = BuildJoinedSchema(left_rel.schema, right_rel.schema);
        bound.has_join = true;
    }

    auto collect_conjuncts = [&](const ExprAST *expr, auto &&self, std::vector<const ExprAST *> *out) -> void {
        if (expr == nullptr) return;
        auto *bin = dynamic_cast<const BinaryExprAST *>(expr);
        if (bin != nullptr && UpperCopy(bin->GetOp()) == "AND") {
            self(bin->GetLeft(), self, out);
            self(bin->GetRight(), self, out);
            return;
        }
        out->push_back(expr);
    };

    auto collect_referenced_table_indexes = [&](const ExprAST *expr, auto &&self, std::unordered_set<std::size_t> *out) -> void {
        if (expr == nullptr) return;
        if (auto *col = dynamic_cast<const ColumnRefExprAST *>(expr)) {
            bool found = false;
            std::size_t found_idx = 0;
            for (std::size_t table_idx = 0; table_idx < bound.tables.size(); table_idx++) {
                const auto &table = bound.tables[table_idx];
                if (!col->GetQualifier().empty() && col->GetQualifier() != table.alias &&
                    col->GetQualifier() != table.table_name) {
                    continue;
                }
                for (std::size_t i = 0; i < table.relation->schema.GetColumnCount(); i++) {
                    if (table.relation->schema.GetColumn(i).GetName() == col->GetColumnName()) {
                        if (found && found_idx != table_idx) {
                            throw std::runtime_error("Ambiguous column reference: " + col->GetColumnName());
                        }
                        found = true;
                        found_idx = table_idx;
                    }
                }
            }
            if (!found) {
                throw std::runtime_error("Unknown column reference: " + col->GetColumnName());
            }
            out->insert(found_idx);
            return;
        }
        if (auto *bin = dynamic_cast<const BinaryExprAST *>(expr)) {
            self(bin->GetLeft(), self, out);
            self(bin->GetRight(), self, out);
            return;
        }
        if (auto *fn = dynamic_cast<const FunctionCallExprAST *>(expr)) {
            if (!fn->IsStarArg() && fn->GetArg() != nullptr) self(fn->GetArg(), self, out);
            return;
        }
    };

    if (stmt.GetWherePredicate() != nullptr) {
        std::vector<const ExprAST *> conjuncts;
        collect_conjuncts(stmt.GetWherePredicate(), collect_conjuncts, &conjuncts);

        std::vector<const ExprAST *> left_pushdown;
        std::vector<const ExprAST *> right_pushdown;
        std::vector<const ExprAST *> residual_conjuncts;

        for (const ExprAST *conj : conjuncts) {
            if (!bound.has_join) {
                left_pushdown.push_back(conj);
                continue;
            }
            std::unordered_set<std::size_t> refs;
            collect_referenced_table_indexes(conj, collect_referenced_table_indexes, &refs);
            if (refs.size() == 1) {
                if (*refs.begin() == 0) left_pushdown.push_back(conj);
                else right_pushdown.push_back(conj);
            } else {
                residual_conjuncts.push_back(conj);
            }
        }

        auto bind_pushdown_predicates = [&](const std::vector<const ExprAST *> &conjs,
                                            const BoundSelectStatement::TableBinding &table_binding,
                                            const Schema &schema) -> std::unique_ptr<AbstractExpression> {
            std::unique_ptr<AbstractExpression> expr;
            BoundSelectStatement::TableBinding local_binding = table_binding;
            local_binding.offset = 0;
            std::vector<BoundSelectStatement::TableBinding> one_table{local_binding};
            for (const ExprAST *conj : conjs) {
                auto next = BindBooleanSourceExpr(*conj, one_table, schema);
                if (!expr) expr = std::move(next);
                else expr = std::make_unique<ConjunctionExpression>(std::move(expr), std::move(next));
            }
            return expr;
        };

        if (!left_pushdown.empty()) {
            auto pred = bind_pushdown_predicates(left_pushdown, left_table, left_rel.schema);
            left_logical = std::make_unique<LogicalFilterNode>(std::move(left_logical), std::move(pred), left_rel.schema);
        }
        if (bound.has_join && !right_pushdown.empty()) {
            auto pred = bind_pushdown_predicates(right_pushdown, *right_table_opt, right_rel_ptr->schema);
            right_logical = std::make_unique<LogicalFilterNode>(std::move(right_logical), std::move(pred), right_rel_ptr->schema);
        }

        if (!residual_conjuncts.empty()) {
            std::unique_ptr<AbstractExpression> residual_expr;
            for (const ExprAST *conj : residual_conjuncts) {
                auto next = BindBooleanSourceExpr(*conj, bound.tables, bound.source_schema);
                if (!residual_expr) residual_expr = std::move(next);
                else residual_expr = std::make_unique<ConjunctionExpression>(std::move(residual_expr), std::move(next));
            }
            bound.where_predicate = std::move(residual_expr);
        } else if (!bound.has_join && !left_pushdown.empty()) {
            bound.where_predicate = bind_pushdown_predicates(left_pushdown, left_table, left_rel.schema);
        }
    }

    if (bound.has_join) {
        const auto *join_bin = dynamic_cast<const BinaryExprAST *>(stmt.GetJoinPredicate());
        if (join_bin == nullptr || join_bin->GetOp() != "=") {
            throw std::runtime_error("Only simple equi-join predicates are supported");
        }
        BoundExprInfo join_left = BindSourceExpr(*join_bin->GetLeft(), bound.tables, bound.source_schema);
        BoundExprInfo join_right = BindSourceExpr(*join_bin->GetRight(), bound.tables, bound.source_schema);
        if (!join_left.is_simple_column || !join_right.is_simple_column) {
            throw std::runtime_error("JOIN ON must be column = column");
        }
        bool left_on_left_table = join_left.source_column_idx < left_rel.schema.GetColumnCount();
        bool right_on_left_table = join_right.source_column_idx < left_rel.schema.GetColumnCount();
        if (left_on_left_table == right_on_left_table) {
            throw std::runtime_error("JOIN ON must compare one left-table column and one right-table column");
        }
        std::size_t left_key_idx = left_on_left_table ? join_left.source_column_idx : join_right.source_column_idx;
        std::size_t right_key_idx = left_on_left_table
                                        ? (join_right.source_column_idx - left_rel.schema.GetColumnCount())
                                        : (join_left.source_column_idx - left_rel.schema.GetColumnCount());
        bound.left_join_key_expr = std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, left_key_idx);
        bound.right_join_key_expr = std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, right_key_idx);
        auto join_pred_for_logical = std::make_unique<ComparisonExpression>(
            ComparisonType::EQ,
            std::make_unique<ColumnValueExpression>(TupleSide::LEFT, left_key_idx),
            std::make_unique<ColumnValueExpression>(TupleSide::RIGHT, right_key_idx));
        logical_root = std::make_unique<LogicalJoinNode>(
            std::move(left_logical), std::move(right_logical), std::move(join_pred_for_logical), bound.source_schema);
    } else {
        logical_root = std::move(left_logical);
    }

    bool uses_star = false;
    bool has_aggregate_fn = false;
    bool has_group_by = !stmt.GetGroupByExprs().empty();
    for (const auto &item : stmt.GetSelectItems()) {
        if (item.is_star) uses_star = true;
        if (dynamic_cast<const FunctionCallExprAST *>(item.expr.get()) != nullptr) has_aggregate_fn = true;
    }

    bound.distinct = stmt.IsDistinct();
    bound.dedup_via_group_by = has_group_by && !has_aggregate_fn;
    bound.requires_dedup = bound.distinct || bound.dedup_via_group_by;

    if (uses_star && (has_group_by || has_aggregate_fn)) {
        throw std::runtime_error("SELECT * cannot be mixed with GROUP BY / aggregates in this frontend");
    }

    if (!has_aggregate_fn) {
        std::unordered_map<std::size_t, std::size_t> group_position_by_source_idx;
        if (has_group_by) {
            for (const auto &group_expr_ast : stmt.GetGroupByExprs()) {
                BoundExprInfo g = BindSourceExpr(*group_expr_ast, bound.tables, bound.source_schema);
                if (!g.is_simple_column) {
                    throw std::runtime_error("GROUP BY currently supports only column references");
                }
                if (group_position_by_source_idx.count(g.source_column_idx) == 0) {
                    group_position_by_source_idx[g.source_column_idx] = group_position_by_source_idx.size();
                }
            }
        }

        std::vector<Column> out_cols;
        std::vector<std::unique_ptr<AbstractExpression>> project_exprs;
        std::vector<std::optional<std::size_t>> project_source_indexes;
        std::unordered_map<std::string, std::size_t> alias_to_project_idx;

        if (uses_star) {
            for (std::size_t i = 0; i < bound.source_schema.GetColumnCount(); i++) {
                out_cols.push_back(bound.source_schema.GetColumn(i));
                project_exprs.push_back(std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, i));
                project_source_indexes.push_back(i);
                alias_to_project_idx.emplace(bound.source_schema.GetColumn(i).GetName(), i);
            }
        } else {
            for (std::size_t i = 0; i < stmt.GetSelectItems().size(); i++) {
                const auto &item = stmt.GetSelectItems()[i];
                BoundExprInfo bound_expr = BindSourceExpr(*item.expr, bound.tables, bound.source_schema);
                if (has_group_by) {
                    if (!bound_expr.is_simple_column) {
                        throw std::runtime_error("Non-aggregate SELECT items in grouped queries must be grouped columns");
                    }
                    if (group_position_by_source_idx.count(bound_expr.source_column_idx) == 0) {
                        throw std::runtime_error("Non-aggregate SELECT item must appear in GROUP BY");
                    }
                }
                std::string out_name = item.alias.empty() ? DefaultSelectItemName(*item.expr) : item.alias;
                out_cols.emplace_back(out_name, bound_expr.type, true);
                project_source_indexes.push_back(bound_expr.is_simple_column ? std::optional<std::size_t>(bound_expr.source_column_idx)
                                                                            : std::nullopt);
                project_exprs.push_back(std::move(bound_expr.expr));
                alias_to_project_idx.emplace(out_name, i);
            }
        }

        bound.project_exprs = std::move(project_exprs);
        bound.output_schema = Schema(std::move(out_cols));
        bound.order_by_on_output = bound.requires_dedup;
        logical_root = std::make_unique<LogicalProjectNode>(
            std::move(logical_root), std::vector<std::unique_ptr<AbstractExpression>>{}, bound.output_schema);

        for (const auto &item : stmt.GetOrderByItems()) {
            BoundExprInfo order_expr;
            bool bound_ok = false;

            if (auto col = dynamic_cast<const ColumnRefExprAST *>(item.expr.get())) {
                if (col->GetQualifier().empty()) {
                    auto alias_it = alias_to_project_idx.find(col->GetColumnName());
                    if (alias_it != alias_to_project_idx.end()) {
                        if (bound.order_by_on_output) {
                            std::size_t output_idx = alias_it->second;
                            order_expr.expr = std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, output_idx);
                            order_expr.type = bound.output_schema.GetColumn(output_idx).GetType();
                            order_expr.is_simple_column = true;
                            order_expr.source_column_idx = output_idx;
                            order_expr.display_name = col->GetColumnName();
                            bound_ok = true;
                        } else {
                            if (!project_source_indexes[alias_it->second].has_value()) {
                                throw std::runtime_error(
                                    "ORDER BY alias currently requires the aliased SELECT item to be a source column");
                            }
                            std::size_t source_idx = *project_source_indexes[alias_it->second];
                            order_expr.expr = std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, source_idx);
                            order_expr.type = bound.source_schema.GetColumn(source_idx).GetType();
                            order_expr.is_simple_column = true;
                            order_expr.source_column_idx = source_idx;
                            order_expr.display_name = col->GetColumnName();
                            bound_ok = true;
                        }
                    }
                }
            }

            if (!bound_ok) {
                order_expr = bound.order_by_on_output ? BindOutputExpr(*item.expr, bound.output_schema)
                                                      : BindSourceExpr(*item.expr, bound.tables, bound.source_schema);
            }

            SortKeySpec spec;
            spec.expr = std::move(order_expr.expr);
            spec.ascending = item.ascending;
            bound.order_by.push_back(std::move(spec));
        }

        if (!bound.order_by.empty()) {
            logical_root = std::make_unique<LogicalSortNode>(
                std::move(logical_root), std::vector<LogicalSortKey>{}, bound.output_schema);
        }

        bound.logical_plan = std::move(logical_root);
        return bound;
    }

    bound.has_aggregate = true;
    bound.order_by_on_output = true;
    std::vector<std::size_t> group_source_indexes;
    std::unordered_map<std::size_t, std::size_t> group_position_by_source_idx;
    std::vector<Column> aggregate_cols;

    for (const auto &group_expr_ast : stmt.GetGroupByExprs()) {
        BoundExprInfo g = BindSourceExpr(*group_expr_ast, bound.tables, bound.source_schema);
        if (!g.is_simple_column) {
            throw std::runtime_error("GROUP BY currently supports only column references");
        }
        if (group_position_by_source_idx.count(g.source_column_idx) == 0) {
            group_position_by_source_idx[g.source_column_idx] = group_source_indexes.size();
            group_source_indexes.push_back(g.source_column_idx);
            aggregate_cols.emplace_back(g.display_name, g.type, true);
            bound.group_by_exprs.push_back(std::move(g.expr));
        }
    }

    std::vector<Column> final_output_cols;
    std::vector<std::unique_ptr<AbstractExpression>> final_project_exprs;
    std::vector<AggregateType> agg_types;
    std::vector<std::unique_ptr<AbstractExpression>> agg_input_exprs;
    std::unordered_map<std::string, std::size_t> output_alias_to_idx;

    for (const auto &item : stmt.GetSelectItems()) {
        if (item.is_star) throw std::runtime_error("SELECT * is not allowed in aggregate queries");

        if (auto fn = dynamic_cast<const FunctionCallExprAST *>(item.expr.get())) {
            AggregateType agg_type = ParseAggregateType(fn->GetName());
            std::string out_name = item.alias.empty() ? DefaultSelectItemName(*item.expr) : item.alias;
            agg_types.push_back(agg_type);

            TypeId agg_output_type = TypeId::INVALID;
            if (fn->IsStarArg()) {
                if (agg_type != AggregateType::COUNT) {
                    throw std::runtime_error("Only COUNT(*) is supported for star aggregate arguments");
                }
                agg_input_exprs.push_back(nullptr);
                agg_output_type = InferAggregateOutputType(agg_type, TypeId::INVALID, true);
            } else {
                BoundExprInfo agg_input = BindSourceExpr(*fn->GetArg(), bound.tables, bound.source_schema);
                agg_output_type = InferAggregateOutputType(agg_type, agg_input.type, false);
                agg_input_exprs.push_back(std::move(agg_input.expr));
            }

            std::size_t agg_output_idx = group_source_indexes.size() + agg_types.size() - 1;
            final_project_exprs.push_back(std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, agg_output_idx));
            final_output_cols.emplace_back(out_name, agg_output_type, true);
            aggregate_cols.emplace_back(out_name, agg_output_type, true);
            output_alias_to_idx.emplace(out_name, final_output_cols.size() - 1);
            continue;
        }

        BoundExprInfo scalar = BindSourceExpr(*item.expr, bound.tables, bound.source_schema);
        if (!scalar.is_simple_column) {
            throw std::runtime_error("Non-aggregate SELECT items in grouped queries must be grouped columns");
        }
        auto it = group_position_by_source_idx.find(scalar.source_column_idx);
        if (it == group_position_by_source_idx.end()) {
            throw std::runtime_error("Non-aggregate SELECT item must appear in GROUP BY");
        }
        std::size_t output_idx = it->second;
        std::string out_name = item.alias.empty() ? scalar.display_name : item.alias;
        final_project_exprs.push_back(std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, output_idx));
        final_output_cols.emplace_back(out_name, scalar.type, true);
        output_alias_to_idx.emplace(out_name, final_output_cols.size() - 1);
    }

    bound.agg_types = std::move(agg_types);
    bound.agg_input_exprs = std::move(agg_input_exprs);
    bound.aggregate_schema = Schema(std::move(aggregate_cols));
    bound.project_exprs = std::move(final_project_exprs);
    bound.output_schema = Schema(std::move(final_output_cols));

    logical_root = std::make_unique<LogicalAggregateNode>(
        std::move(logical_root),
        std::vector<std::unique_ptr<AbstractExpression>>{},
        std::vector<LogicalAggregateType>{},
        std::vector<std::unique_ptr<AbstractExpression>>{},
        bound.aggregate_schema);
    logical_root = std::make_unique<LogicalProjectNode>(
        std::move(logical_root), std::vector<std::unique_ptr<AbstractExpression>>{}, bound.output_schema);

    for (const auto &item : stmt.GetOrderByItems()) {
        BoundExprInfo order_expr;
        bool bound_ok = false;

        if (auto col = dynamic_cast<const ColumnRefExprAST *>(item.expr.get())) {
            if (col->GetQualifier().empty()) {
                auto alias_it = output_alias_to_idx.find(col->GetColumnName());
                if (alias_it != output_alias_to_idx.end()) {
                    std::size_t idx = alias_it->second;
                    order_expr.expr = std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, idx);
                    order_expr.type = bound.output_schema.GetColumn(idx).GetType();
                    order_expr.is_simple_column = true;
                    order_expr.source_column_idx = idx;
                    order_expr.display_name = bound.output_schema.GetColumn(idx).GetName();
                    bound_ok = true;
                }
            }
        }

        if (!bound_ok) {
            order_expr = BindOutputExpr(*item.expr, bound.output_schema);
        }

        SortKeySpec spec;
        spec.expr = std::move(order_expr.expr);
        spec.ascending = item.ascending;
        bound.order_by.push_back(std::move(spec));
    }

    if (!bound.order_by.empty()) {
        logical_root = std::make_unique<LogicalSortNode>(
            std::move(logical_root), std::vector<LogicalSortKey>{}, bound.output_schema);
    }

    bound.logical_plan = std::move(logical_root);
    return bound;
}


BoundCompoundSelectStatement SqlBinder::BindCompoundSelect(const CompoundSelectStatementAST &stmt) const {
    BoundCompoundSelectStatement bound;
    if (stmt.GetTerms().empty()) {
        throw std::runtime_error("Compound SELECT requires at least one input");
    }

    for (const auto &term : stmt.GetTerms()) {
        bound.inputs.push_back(BindSelect(*term));
    }
    for (const auto &op : stmt.GetOperations()) {
        bound.operations.push_back(BoundSetOperationSpec{op.type, op.all});
    }

    const Schema &first_schema = bound.inputs.front().output_schema;
    for (std::size_t i = 1; i < bound.inputs.size(); i++) {
        const Schema &other = bound.inputs[i].output_schema;
        if (other.GetColumnCount() != first_schema.GetColumnCount()) {
            throw std::runtime_error("All branches of a set operation must return the same number of columns");
        }
        for (std::size_t col = 0; col < first_schema.GetColumnCount(); col++) {
            if (other.GetColumn(col).GetType() != first_schema.GetColumn(col).GetType()) {
                throw std::runtime_error("Set operation column types must match exactly in this frontend");
            }
        }
    }
    bound.output_schema = first_schema;

    for (const auto &item : stmt.GetOrderByItems()) {
        BoundExprInfo order_expr = BindOutputExpr(*item.expr, bound.output_schema);
        SortKeySpec spec;
        spec.expr = std::move(order_expr.expr);
        spec.ascending = item.ascending;
        bound.order_by.push_back(std::move(spec));
    }
    bound.limit = stmt.GetLimit();
    return bound;
}

}  // namespace simpledb
