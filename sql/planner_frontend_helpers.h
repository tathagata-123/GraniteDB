#pragma once

#include "planner_frontend.h"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace simpledb::planner_frontend_helpers {

// These helpers are intentionally kept outside the SqlPlannerFrontend class.
// They do not touch global frontend state. Keeping them here makes the actual
// planner/executor orchestration code much easier to read because the member
// functions can focus on "what stage is happening now" instead of also carrying
// every tiny expression-tree utility inline.

std::string RowsMessage(const std::string &prefix, std::size_t n);
std::vector<std::unique_ptr<AbstractExpression>> CloneExprVector(
    const std::vector<std::unique_ptr<AbstractExpression>> &exprs);
bool IsIdentityProjection(const std::vector<std::unique_ptr<AbstractExpression>> &exprs,
                          const Schema &input_schema);
std::vector<SortKeySpec> FullRowSortKeys(const Schema &schema);
void CollectColumnRefs(const AbstractExpression *expr, std::unordered_set<std::size_t> *out);
bool ExtractSingleTablePredicate(const AbstractExpression *expr, QueryPredicateSpec *out);
void CollectConjunctiveSingleTablePredicates(const AbstractExpression *expr,
                                             std::vector<QueryPredicateSpec> *out);
bool CollectConjunctiveBitmapTerms(const AbstractExpression *expr,
                                   std::vector<QueryPredicateSpec> *out);
bool CollectBitmapDnfGroups(const AbstractExpression *expr,
                            std::vector<std::vector<QueryPredicateSpec>> *groups);
std::vector<Value> BuildIndexKeyValues(const Tuple &tuple,
                                       const std::vector<IndexKeyColumnDefinition> &key_columns);
void ValidateIndexBuildKeyValues(const std::string &index_name,
                                 const std::vector<Value> &key_values,
                                 NullPolicy null_policy);
std::unique_ptr<AbstractExecutor> MakeBitmapLeaf(const IndexCatalogEntry *index,
                                                 const QueryPredicateSpec &pred);
std::unordered_set<std::size_t> CollectReferencedSourceColumns(const BoundSelectStatement &stmt);

}  // namespace simpledb::planner_frontend_helpers
