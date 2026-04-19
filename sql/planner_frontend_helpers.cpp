#include "planner_frontend_helpers.h"

#include <sstream>
#include <stdexcept>

namespace simpledb::planner_frontend_helpers {

std::string RowsMessage(const std::string &prefix, std::size_t n) {
    std::ostringstream out;
    out << prefix << " " << n << " row" << (n == 1 ? "" : "s") << ".";
    return out.str();
}

std::vector<std::unique_ptr<AbstractExpression>> CloneExprVector(
    const std::vector<std::unique_ptr<AbstractExpression>> &exprs) {
    std::vector<std::unique_ptr<AbstractExpression>> out;
    out.reserve(exprs.size());
    for (const auto &expr : exprs) {
        if (expr == nullptr) out.push_back(nullptr);
        else out.push_back(std::unique_ptr<AbstractExpression>(expr->Clone()));
    }
    return out;
}

bool IsIdentityProjection(const std::vector<std::unique_ptr<AbstractExpression>> &exprs,
                         const Schema &input_schema) {
    if (exprs.size() != input_schema.GetColumnCount()) return false;
    for (std::size_t i = 0; i < exprs.size(); i++) {
        auto *col = dynamic_cast<ColumnValueExpression *>(exprs[i].get());
        if (col == nullptr || col->GetSide() != TupleSide::SINGLE || col->GetColumnIdx() != i) return false;
    }
    return true;
}

std::vector<SortKeySpec> FullRowSortKeys(const Schema &schema) {
    std::vector<SortKeySpec> keys;
    keys.reserve(schema.GetColumnCount());
    for (std::size_t i = 0; i < schema.GetColumnCount(); i++) {
        SortKeySpec spec;
        spec.expr = std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, i);
        spec.ascending = true;
        keys.push_back(std::move(spec));
    }
    return keys;
}

void CollectColumnRefs(const AbstractExpression *expr, std::unordered_set<std::size_t> *out) {
    if (expr == nullptr) return;
    if (auto col = dynamic_cast<const ColumnValueExpression *>(expr)) {
        if (col->GetSide() == TupleSide::SINGLE) out->insert(col->GetColumnIdx());
        return;
    }
    if (auto cmp = dynamic_cast<const ComparisonExpression *>(expr)) {
        CollectColumnRefs(cmp->GetLeft(), out);
        CollectColumnRefs(cmp->GetRight(), out);
        return;
    }
    if (auto conj = dynamic_cast<const ConjunctionExpression *>(expr)) {
        CollectColumnRefs(conj->GetLeft(), out);
        CollectColumnRefs(conj->GetRight(), out);
        return;
    }
    if (auto disj = dynamic_cast<const DisjunctionExpression *>(expr)) {
        CollectColumnRefs(disj->GetLeft(), out);
        CollectColumnRefs(disj->GetRight(), out);
        return;
    }
    if (auto arith = dynamic_cast<const ArithmeticExpression *>(expr)) {
        CollectColumnRefs(arith->GetLeft(), out);
        CollectColumnRefs(arith->GetRight(), out);
        return;
    }
}

bool ExtractSingleTablePredicate(const AbstractExpression *expr,
                                 QueryPredicateSpec *out) {
    auto cmp = dynamic_cast<const ComparisonExpression *>(expr);
    if (cmp == nullptr) return false;
    auto col = dynamic_cast<const ColumnValueExpression *>(cmp->GetLeft());
    auto cst = dynamic_cast<const ConstantValueExpression *>(cmp->GetRight());
    if (col == nullptr || cst == nullptr || col->GetSide() != TupleSide::SINGLE) return false;
    out->column_idx = col->GetColumnIdx();
    out->cmp = cmp->GetComparisonType();
    out->constant = cst->GetValue();
    return true;
}

void CollectConjunctiveSingleTablePredicates(const AbstractExpression *expr,
                                             std::vector<QueryPredicateSpec> *out) {
    if (expr == nullptr) return;
    if (auto conj = dynamic_cast<const ConjunctionExpression *>(expr)) {
        CollectConjunctiveSingleTablePredicates(conj->GetLeft(), out);
        CollectConjunctiveSingleTablePredicates(conj->GetRight(), out);
        return;
    }
    QueryPredicateSpec pred;
    if (ExtractSingleTablePredicate(expr, &pred)) out->push_back(pred);
}

bool CollectConjunctiveBitmapTerms(const AbstractExpression *expr,
                                   std::vector<QueryPredicateSpec> *out) {
    if (expr == nullptr) return true;
    if (auto conj = dynamic_cast<const ConjunctionExpression *>(expr)) {
        return CollectConjunctiveBitmapTerms(conj->GetLeft(), out) &&
               CollectConjunctiveBitmapTerms(conj->GetRight(), out);
    }
    if (dynamic_cast<const DisjunctionExpression *>(expr) != nullptr) return false;
    QueryPredicateSpec pred;
    if (!ExtractSingleTablePredicate(expr, &pred)) return false;
    out->push_back(std::move(pred));
    return true;
}

bool CollectBitmapDnfGroups(const AbstractExpression *expr,
                            std::vector<std::vector<QueryPredicateSpec>> *groups) {
    if (expr == nullptr) return false;
    if (auto disj = dynamic_cast<const DisjunctionExpression *>(expr)) {
        return CollectBitmapDnfGroups(disj->GetLeft(), groups) &&
               CollectBitmapDnfGroups(disj->GetRight(), groups);
    }
    std::vector<QueryPredicateSpec> group;
    if (!CollectConjunctiveBitmapTerms(expr, &group) || group.empty()) return false;
    groups->push_back(std::move(group));
    return true;
}

std::vector<Value> BuildIndexKeyValues(const Tuple &tuple,
                                       const std::vector<IndexKeyColumnDefinition> &key_columns) {
    std::vector<Value> key_values;
    key_values.reserve(key_columns.size());
    for (const auto &key_def : key_columns) {
        key_values.push_back(tuple.GetValue(key_def.column_idx));
    }
    return key_values;
}

void ValidateIndexBuildKeyValues(const std::string &index_name,
                                 const std::vector<Value> &key_values,
                                 NullPolicy null_policy) {
    if (null_policy != NullPolicy::NOT_SUPPORTED) {
        throw std::runtime_error("Only NOT_SUPPORTED null policy is implemented for CREATE INDEX");
    }
    for (const Value &value : key_values) {
        if (value.IsNull()) {
            throw std::runtime_error(
                "CREATE INDEX failed: NULL index keys are not supported by this storage/index layer: " +
                index_name);
        }
    }
}

std::unique_ptr<AbstractExecutor> MakeBitmapLeaf(const IndexCatalogEntry *index,
                                                 const QueryPredicateSpec &pred) {
    if (index == nullptr || index->GetBTreeIndex() == nullptr) return nullptr;
    std::optional<Value> equality_key;
    std::optional<Value> lower;
    std::optional<Value> upper;
    bool lower_inclusive = true;
    bool upper_inclusive = true;
    if (pred.cmp == ComparisonType::EQ) {
        equality_key = pred.constant;
    } else if (pred.cmp == ComparisonType::GT || pred.cmp == ComparisonType::GTE) {
        lower = pred.constant;
        lower_inclusive = (pred.cmp == ComparisonType::GTE);
    } else if (pred.cmp == ComparisonType::LT || pred.cmp == ComparisonType::LTE) {
        upper = pred.constant;
        upper_inclusive = (pred.cmp == ComparisonType::LTE);
    } else {
        return nullptr;
    }
    return std::make_unique<BitmapIndexScanExecutor>(
        index->GetBTreeIndex(), equality_key, lower, upper, lower_inclusive, upper_inclusive);
}

std::unordered_set<std::size_t> CollectReferencedSourceColumns(const BoundSelectStatement &stmt) {
    std::unordered_set<std::size_t> cols;
    for (const auto &expr : stmt.project_exprs) CollectColumnRefs(expr.get(), &cols);
    CollectColumnRefs(stmt.where_predicate.get(), &cols);
    for (const auto &key : stmt.order_by) CollectColumnRefs(key.expr.get(), &cols);
    for (const auto &expr : stmt.group_by_exprs) CollectColumnRefs(expr.get(), &cols);
    for (const auto &expr : stmt.agg_input_exprs) CollectColumnRefs(expr.get(), &cols);
    return cols;
}

}  // namespace simpledb::planner_frontend_helpers
