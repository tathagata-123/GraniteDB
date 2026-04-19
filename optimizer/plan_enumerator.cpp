#include "plan_enumerator.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace simpledb {
namespace {

bool ExtractBasePredicate(const AbstractExpression *expr,
                          RelationId relation_id,
                          QueryPredicateSpec *out) {
    auto cmp = dynamic_cast<const ComparisonExpression *>(expr);
    if (cmp == nullptr) return false;
    auto col = dynamic_cast<const ColumnValueExpression *>(cmp->GetLeft());
    auto cst = dynamic_cast<const ConstantValueExpression *>(cmp->GetRight());
    if (col == nullptr || cst == nullptr || col->GetSide() != TupleSide::SINGLE) return false;
    out->relation_id = relation_id;
    out->column_idx = col->GetColumnIdx();
    out->cmp = cmp->GetComparisonType();
    out->constant = cst->GetValue();
    return true;
}

bool ExtractJoinPredicate(const AbstractExpression *expr,
                          RelationId left_relation_id,
                          RelationId right_relation_id,
                          QueryJoinPredicateSpec *out) {
    auto cmp = dynamic_cast<const ComparisonExpression *>(expr);
    if (cmp == nullptr || cmp->GetComparisonType() != ComparisonType::EQ) return false;
    auto left_col = dynamic_cast<const ColumnValueExpression *>(cmp->GetLeft());
    auto right_col = dynamic_cast<const ColumnValueExpression *>(cmp->GetRight());
    if (left_col == nullptr || right_col == nullptr) return false;

    if (left_col->GetSide() == TupleSide::LEFT && right_col->GetSide() == TupleSide::RIGHT) {
        out->left_relation_id = left_relation_id;
        out->left_column_idx = left_col->GetColumnIdx();
        out->right_relation_id = right_relation_id;
        out->right_column_idx = right_col->GetColumnIdx();
        return true;
    }
    if (left_col->GetSide() == TupleSide::RIGHT && right_col->GetSide() == TupleSide::LEFT) {
        out->left_relation_id = left_relation_id;
        out->left_column_idx = right_col->GetColumnIdx();
        out->right_relation_id = right_relation_id;
        out->right_column_idx = left_col->GetColumnIdx();
        return true;
    }
    return false;
}

void CollectConjunctiveBasePredicates(const AbstractExpression *expr,
                                      RelationId relation_id,
                                      std::vector<QueryPredicateSpec> *out) {
    if (expr == nullptr) return;
    if (auto conj = dynamic_cast<const ConjunctionExpression *>(expr)) {
        CollectConjunctiveBasePredicates(conj->GetLeft(), relation_id, out);
        CollectConjunctiveBasePredicates(conj->GetRight(), relation_id, out);
        return;
    }
    QueryPredicateSpec p;
    if (ExtractBasePredicate(expr, relation_id, &p)) out->push_back(std::move(p));
}

void CollectLogicalQuery(const LogicalPlanNode &node,
                         std::vector<RelationId> *relations,
                         std::vector<QueryPredicateSpec> *predicates,
                         std::vector<QueryJoinPredicateSpec> *joins) {
    switch (node.GetType()) {
        case LogicalNodeType::GET: {
            const auto &get = dynamic_cast<const LogicalGetNode &>(node);
            relations->push_back(get.GetRelationId());
            return;
        }
        case LogicalNodeType::FILTER: {
            const auto &filter = dynamic_cast<const LogicalFilterNode &>(node);
            CollectLogicalQuery(*filter.GetChild(), relations, predicates, joins);
            const LogicalPlanNode *child = filter.GetChild();
            if (child->GetType() != LogicalNodeType::GET) {
                throw std::runtime_error("OptimizeLogicalPlan currently supports FILTER only on base GET nodes");
            }
            RelationId rid = dynamic_cast<const LogicalGetNode *>(child)->GetRelationId();
            CollectConjunctiveBasePredicates(filter.GetPredicate(), rid, predicates);
            return;
        }
        case LogicalNodeType::JOIN: {
            const auto &join = dynamic_cast<const LogicalJoinNode &>(node);
            CollectLogicalQuery(*join.GetLeft(), relations, predicates, joins);
            CollectLogicalQuery(*join.GetRight(), relations, predicates, joins);

            RelationId left_rel = 0;
            RelationId right_rel = 0;
            const LogicalPlanNode *left = join.GetLeft();
            const LogicalPlanNode *right = join.GetRight();
            if (left->GetType() == LogicalNodeType::GET) left_rel = dynamic_cast<const LogicalGetNode *>(left)->GetRelationId();
            else if (left->GetType() == LogicalNodeType::FILTER) left_rel = dynamic_cast<const LogicalGetNode *>(dynamic_cast<const LogicalFilterNode *>(left)->GetChild())->GetRelationId();
            if (right->GetType() == LogicalNodeType::GET) right_rel = dynamic_cast<const LogicalGetNode *>(right)->GetRelationId();
            else if (right->GetType() == LogicalNodeType::FILTER) right_rel = dynamic_cast<const LogicalGetNode *>(dynamic_cast<const LogicalFilterNode *>(right)->GetChild())->GetRelationId();
            if (left_rel == 0 || right_rel == 0) {
                throw std::runtime_error("OptimizeLogicalPlan currently supports joins whose inputs resolve to base relations");
            }
            QueryJoinPredicateSpec jp;
            if (!ExtractJoinPredicate(join.GetPredicate(), left_rel, right_rel, &jp)) {
                throw std::runtime_error("OptimizeLogicalPlan only supports simple equi-join predicates between LEFT and RIGHT columns");
            }
            joins->push_back(jp);
            return;
        }
        case LogicalNodeType::PROJECT: {
            const auto &proj = dynamic_cast<const LogicalProjectNode &>(node);
            CollectLogicalQuery(*proj.GetChild(), relations, predicates, joins);
            return;
        }
        case LogicalNodeType::SORT: {
            const auto &sort = dynamic_cast<const LogicalSortNode &>(node);
            CollectLogicalQuery(*sort.GetChild(), relations, predicates, joins);
            return;
        }
        case LogicalNodeType::AGGREGATE: {
            const auto &agg = dynamic_cast<const LogicalAggregateNode &>(node);
            CollectLogicalQuery(*agg.GetChild(), relations, predicates, joins);
            return;
        }
        default:
            throw std::runtime_error("OptimizeLogicalPlan currently supports only GET/FILTER/JOIN/PROJECT/SORT/AGGREGATE logical nodes");
    }
}

std::vector<PlanOrderKey> BuildIndexOrderKeys(RelationId relation_id,
                                              const IndexCatalogEntry &index) {
    std::vector<PlanOrderKey> out;
    out.reserve(index.key_columns.size());
    for (const auto &key_col : index.key_columns) {
        out.push_back(PlanOrderKey{relation_id, key_col.column_idx, true});
    }
    return out;
}

bool IsLowerBoundCmp(ComparisonType cmp) {
    return cmp == ComparisonType::GT || cmp == ComparisonType::GTE;
}

bool IsUpperBoundCmp(ComparisonType cmp) {
    return cmp == ComparisonType::LT || cmp == ComparisonType::LTE;
}

void SetPathCosts(const CostModel::CostEstimate &cost, const std::shared_ptr<PhysicalPath> &path) {
    path->startup_cost = cost.startup_cost;
    path->total_cost = cost.total_cost;
    path->cost = cost.total_cost;
}

std::vector<QueryPredicateSpec> AccessPredicatesAsVector(const IndexAccessSpec &spec) {
    std::vector<QueryPredicateSpec> out = spec.index_predicates;
    if (spec.lower_bound.has_value()) out.push_back(*spec.lower_bound);
    if (spec.upper_bound.has_value()) out.push_back(*spec.upper_bound);
    return out;
}

}  // namespace

PlanEnumerator::PlanEnumerator(const CatalogManager &catalog,
                               const StatsCatalog &stats,
                               CostModel cost_model)
    : catalog_(catalog), stats_(stats), cost_model_(std::move(cost_model)) {}

std::vector<QueryPredicateSpec> PlanEnumerator::GetPredicatesForRelation(
    const OptimizerQuery &query,
    RelationId relation_id) const {
    std::vector<QueryPredicateSpec> out;
    for (const auto &p : query.predicates) {
        if (p.relation_id == relation_id) out.push_back(p);
    }
    return out;
}

std::vector<QueryJoinPredicateSpec> PlanEnumerator::GetJoinsBetweenSubsetAndRelation(
    const OptimizerQuery &query,
    uint64_t subset_mask,
    RelationId relation_id,
    const std::unordered_map<RelationId, std::size_t> &rel_pos) const {
    std::vector<QueryJoinPredicateSpec> result;
    for (const auto &j : query.joins) {
        auto it_l = rel_pos.find(j.left_relation_id);
        auto it_r = rel_pos.find(j.right_relation_id);
        if (it_l == rel_pos.end() || it_r == rel_pos.end()) continue;
        bool left_in_subset = ((subset_mask >> it_l->second) & 1ULL) != 0;
        bool right_in_subset = ((subset_mask >> it_r->second) & 1ULL) != 0;
        if ((left_in_subset && j.right_relation_id == relation_id && !right_in_subset) ||
            (right_in_subset && j.left_relation_id == relation_id && !left_in_subset)) {
            result.push_back(j);
        }
    }
    return result;
}

double PlanEnumerator::EstimatePredicateListSelectivity(const RelationStats &stats,
                                                        const std::vector<QueryPredicateSpec> &predicates) const {
    if (predicates.empty()) return 1.0;
    std::vector<bool> used(predicates.size(), false);
    double sel = 1.0;

    for (std::size_t i = 0; i < predicates.size(); i++) {
        if (used[i]) continue;
        if (!predicates[i].IsEquality()) continue;
        double best_pair_sel = 0.0;
        std::optional<std::size_t> best_j;
        for (std::size_t j = i + 1; j < predicates.size(); j++) {
            if (used[j] || !predicates[j].IsEquality()) continue;
            if (predicates[i].column_idx == predicates[j].column_idx) continue;
            double pair_sel = 0.0;
            if (!cost_model_.TryEstimatePairEqualitySelectivity(
                    stats,
                    predicates[i].column_idx, predicates[i].constant,
                    predicates[j].column_idx, predicates[j].constant,
                    &pair_sel)) {
                continue;
            }
            if (!best_j.has_value() || pair_sel > best_pair_sel) {
                best_pair_sel = pair_sel;
                best_j = j;
            }
        }
        if (best_j.has_value()) {
            used[i] = true;
            used[*best_j] = true;
            sel *= best_pair_sel;
        }
    }

    for (std::size_t i = 0; i < predicates.size(); i++) {
        if (used[i]) continue;
        sel *= cost_model_.EstimatePredicateSelectivity(stats, predicates[i].column_idx, predicates[i].cmp, predicates[i].constant);
    }
    return CostModel::ClampSelectivity(sel);
}

std::vector<std::shared_ptr<PhysicalPath>> PlanEnumerator::MakeBaseCandidates(
    RelationId relation_id,
    uint64_t relation_bit,
    const std::vector<QueryPredicateSpec> &predicates,
    const OptimizerQuery &query) const {
    const auto &rel = catalog_.GetRelation(relation_id);
    const auto &rel_stats = stats_.GetRelationStats(relation_id);
    std::vector<std::shared_ptr<PhysicalPath>> candidates;

    double total_selectivity = EstimatePredicateListSelectivity(rel_stats, predicates);

    auto seq = std::make_shared<PhysicalPath>();
    seq->kind = PhysicalPath::Kind::BASE;
    seq->relation_id = relation_id;
    seq->relation_mask = relation_bit;
    seq->output_schema = rel.schema;
    seq->relation_offsets[relation_id] = 0;
    seq->all_predicates = predicates;
    seq->access_spec.residual_predicates = predicates;
    seq->access_type = AccessPathType::SEQ_SCAN;
    seq->est_rows = std::max(1.0, total_selectivity * static_cast<double>(rel_stats.tuple_count));
    SetPathCosts(cost_model_.EstimateSeqScanCost(rel_stats, total_selectivity, predicates.size()), seq);
    candidates.push_back(seq);

    for (const auto &index : catalog_.GetIndexes(relation_id)) {
        std::unordered_set<std::size_t> used_predicate_indexes;
        IndexAccessSpec access_spec;
        std::size_t equality_prefix_count = 0;
        bool used_any_predicate = false;

        for (std::size_t key_pos = 0; key_pos < index.key_columns.size(); key_pos++) {
            std::size_t column_idx = index.key_columns[key_pos].column_idx;
            std::optional<std::size_t> equality_pred_idx;
            std::optional<std::size_t> best_lower_idx;
            std::optional<std::size_t> best_upper_idx;

            for (std::size_t pred_idx = 0; pred_idx < predicates.size(); pred_idx++) {
                if (used_predicate_indexes.count(pred_idx) != 0) continue;
                const auto &pred = predicates[pred_idx];
                if (pred.column_idx != column_idx) continue;
                if (pred.IsEquality()) {
                    equality_pred_idx = pred_idx;
                    break;
                }
                if (IsLowerBoundCmp(pred.cmp) && !best_lower_idx.has_value()) best_lower_idx = pred_idx;
                if (IsUpperBoundCmp(pred.cmp) && !best_upper_idx.has_value()) best_upper_idx = pred_idx;
            }

            if (equality_pred_idx.has_value()) {
                used_any_predicate = true;
                used_predicate_indexes.insert(*equality_pred_idx);
                access_spec.index_predicates.push_back(predicates[*equality_pred_idx]);
                equality_prefix_count++;
                continue;
            }

            if (best_lower_idx.has_value() || best_upper_idx.has_value()) {
                used_any_predicate = true;
                if (best_lower_idx.has_value()) {
                    used_predicate_indexes.insert(*best_lower_idx);
                    access_spec.lower_bound = predicates[*best_lower_idx];
                }
                if (best_upper_idx.has_value()) {
                    used_predicate_indexes.insert(*best_upper_idx);
                    access_spec.upper_bound = predicates[*best_upper_idx];
                }
                break;
            }
            break;
        }

        access_spec.equality_prefix_count = equality_prefix_count;
        for (std::size_t i = 0; i < predicates.size(); i++) {
            if (used_predicate_indexes.count(i) == 0) access_spec.residual_predicates.push_back(predicates[i]);
        }

        auto candidate = std::make_shared<PhysicalPath>();
        candidate->kind = PhysicalPath::Kind::BASE;
        candidate->relation_id = relation_id;
        candidate->relation_mask = relation_bit;
        candidate->output_schema = rel.schema;
        candidate->relation_offsets[relation_id] = 0;
        candidate->all_predicates = predicates;
        candidate->access_index = &index;
        candidate->access_spec = access_spec;
        candidate->output_order = BuildIndexOrderKeys(relation_id, index);
        for (std::size_t i = 0; i < equality_prefix_count && i < candidate->output_order.size(); i++) {
            candidate->fixed_order_prefix.push_back(candidate->output_order[i]);
        }

        bool order_candidate = !query.required_order.empty() || (query.has_aggregate && !query.group_order.empty()) || query.limit_count.has_value();
        if (!used_any_predicate && !order_candidate) continue;

        auto access_predicates = AccessPredicatesAsVector(access_spec);
        double access_selectivity = access_predicates.empty() ? 1.0 : EstimatePredicateListSelectivity(rel_stats, access_predicates);
        candidate->est_rows = std::max(1.0, total_selectivity * static_cast<double>(rel_stats.tuple_count));

        if (!used_any_predicate) {
            candidate->access_type = AccessPathType::INDEX_SCAN_ORDERED;
            SetPathCosts(cost_model_.EstimateIndexScanCost(rel_stats, 1.0, access_spec.residual_predicates.size(), true), candidate);
        } else if (access_spec.lower_bound.has_value() || access_spec.upper_bound.has_value()) {
            candidate->access_type = AccessPathType::INDEX_SCAN_RANGE;
            SetPathCosts(cost_model_.EstimateIndexScanCost(rel_stats, access_selectivity, access_spec.residual_predicates.size(), true), candidate);
        } else if (index.key_columns.size() == 1) {
            candidate->access_type = AccessPathType::INDEX_SCAN_EQ;
            SetPathCosts(cost_model_.EstimateIndexScanCost(rel_stats, access_selectivity, access_spec.residual_predicates.size(), true), candidate);
        } else {
            candidate->access_type = AccessPathType::INDEX_SCAN_RANGE;
            SetPathCosts(cost_model_.EstimateIndexScanCost(rel_stats, access_selectivity, access_spec.residual_predicates.size(), true), candidate);
        }
        candidates.push_back(candidate);
    }

    return candidates;
}

std::vector<std::shared_ptr<PhysicalPath>> PlanEnumerator::TryBuildJoinPaths(
    const std::shared_ptr<PhysicalPath> &left_path,
    const std::shared_ptr<PhysicalPath> &right_base_path,
    const QueryJoinPredicateSpec &join_pred,
    const OptimizerQuery &query) const {
    (void)query;
    RelationId right_rel_id = right_base_path->relation_id;
    const auto &right_stats = stats_.GetRelationStats(right_rel_id);

    bool left_contains_left_rel = left_path->relation_offsets.count(join_pred.left_relation_id) != 0;
    RelationId left_rel_id = left_contains_left_rel ? join_pred.left_relation_id : join_pred.right_relation_id;
    std::size_t left_col_idx = left_contains_left_rel ? join_pred.left_column_idx : join_pred.right_column_idx;
    std::size_t right_col_idx = left_contains_left_rel ? join_pred.right_column_idx : join_pred.left_column_idx;

    const auto &left_rel_stats = stats_.GetRelationStats(left_rel_id);
    double est_join_rows = cost_model_.EstimateJoinRows(
        left_rel_stats, left_col_idx, left_path->est_rows,
        right_stats, right_col_idx, right_base_path->est_rows);

    auto make_join_path = [&](JoinAlgorithmType algo, const CostModel::CostEstimate &cost) {
        auto path = std::make_shared<PhysicalPath>();
        path->kind = PhysicalPath::Kind::JOIN;
        path->left = left_path;
        path->right = right_base_path;
        path->join_type = algo;
        path->join_predicate = join_pred;
        path->est_rows = est_join_rows;
        path->relation_mask = left_path->relation_mask | right_base_path->relation_mask;
        path->output_schema = ConcatSchemas(left_path->output_schema, right_base_path->output_schema);
        path->relation_offsets = left_path->relation_offsets;
        std::size_t left_cols = left_path->output_schema.GetColumnCount();
        for (const auto &[rid, off] : right_base_path->relation_offsets) {
            path->relation_offsets[rid] = left_cols + off;
        }
        SetPathCosts(cost, path);
        if (algo == JoinAlgorithmType::INDEX_NESTED_LOOP || algo == JoinAlgorithmType::NESTED_LOOP) {
            path->output_order = left_path->output_order;
            path->fixed_order_prefix = left_path->fixed_order_prefix;
        }
        if (algo == JoinAlgorithmType::INDEX_NESTED_LOOP) path->parameterized_like = true;
        return path;
    };

    std::vector<std::shared_ptr<PhysicalPath>> out;
    out.push_back(make_join_path(
        JoinAlgorithmType::NESTED_LOOP,
        cost_model_.EstimateNestedLoopJoinCost(
            left_path->startup_cost, left_path->total_cost,
            right_base_path->startup_cost, right_base_path->total_cost,
            left_path->est_rows, right_base_path->est_rows)));

    out.push_back(make_join_path(
        JoinAlgorithmType::HASH_JOIN,
        cost_model_.EstimateHashJoinCost(
            left_path->startup_cost, left_path->total_cost,
            right_base_path->startup_cost, right_base_path->total_cost,
            left_path->est_rows, right_base_path->est_rows)));

    std::vector<PlanOrderKey> left_join_order = {PlanOrderKey{left_rel_id, left_col_idx, true}};
    std::vector<PlanOrderKey> right_join_order = {PlanOrderKey{right_rel_id, right_col_idx, true}};
    bool left_sorted = left_path->SatisfiesOrder(left_join_order);
    bool right_sorted = right_base_path->SatisfiesOrder(right_join_order);
    auto left_sort = left_sorted ? CostModel::CostEstimate{} : cost_model_.EstimateSortCost(left_path->est_rows);
    auto right_sort = right_sorted ? CostModel::CostEstimate{} : cost_model_.EstimateSortCost(right_base_path->est_rows);
    CostModel::CostEstimate merge_cost = cost_model_.EstimateMergeJoinCost(
        left_path->startup_cost + left_sort.startup_cost,
        left_path->total_cost + left_sort.total_cost,
        right_base_path->startup_cost + right_sort.startup_cost,
        right_base_path->total_cost + right_sort.total_cost,
        left_path->est_rows, right_base_path->est_rows);
    auto merge_path = make_join_path(JoinAlgorithmType::MERGE_JOIN, merge_cost);
    merge_path->merge_left_needs_sort = !left_sorted;
    merge_path->merge_right_needs_sort = !right_sorted;
    merge_path->output_order = left_sorted ? left_path->output_order : left_join_order;
    merge_path->fixed_order_prefix = left_sorted ? left_path->fixed_order_prefix : std::vector<PlanOrderKey>{};
    out.push_back(std::move(merge_path));

    const IndexCatalogEntry *inner_index = catalog_.FindIndexOnColumn(right_rel_id, right_col_idx);
    if (inner_index != nullptr && inner_index->GetBTreeIndex() != nullptr) {
        double inner_lookup_sel = cost_model_.EstimatePredicateSelectivity(
            right_stats, right_col_idx, ComparisonType::EQ, Value(int32_t(0)));
        auto inlj = make_join_path(
            JoinAlgorithmType::INDEX_NESTED_LOOP,
            cost_model_.EstimateIndexNestedLoopJoinCost(
                left_path->startup_cost, left_path->total_cost,
                left_path->est_rows, inner_lookup_sel));
        out.push_back(inlj);

        std::size_t outer_ndv = left_rel_stats.columns.count(left_col_idx) != 0
                                    ? left_rel_stats.columns.at(left_col_idx).distinct_count
                                    : static_cast<std::size_t>(std::max(1.0, left_path->est_rows));
        double distinct_outer_keys = std::max(1.0, std::min(left_path->est_rows, static_cast<double>(std::max<std::size_t>(1, outer_ndv))));
        if (left_path->est_rows > distinct_outer_keys * 1.25) {
            auto memo = make_join_path(
                JoinAlgorithmType::INDEX_NESTED_LOOP,
                cost_model_.EstimateMemoizedIndexNestedLoopJoinCost(
                    left_path->startup_cost, left_path->total_cost,
                    left_path->est_rows, distinct_outer_keys, inner_lookup_sel));
            memo->uses_memoize = true;
            memo->parameterized_like = true;
            out.push_back(std::move(memo));
        }
    }

    return out;
}

std::string PlanEnumerator::SerializeOrder(const std::vector<PlanOrderKey> &order) const {
    std::ostringstream out;
    for (const auto &key : order) {
        out << key.relation_id << ':' << key.column_idx << ':' << (key.ascending ? 'A' : 'D') << ';';
    }
    return out.str();
}

void PlanEnumerator::AddPrunedCandidate(std::vector<std::shared_ptr<PhysicalPath>> *bucket,
                                        std::shared_ptr<PhysicalPath> candidate,
                                        const OptimizerQuery &query) const {
    bucket->push_back(std::move(candidate));

    // First prune exact dominated variants inside the same order/parameterization signature.
    std::vector<std::shared_ptr<PhysicalPath>> nondominated;
    for (std::size_t i = 0; i < bucket->size(); i++) {
        bool dominated = false;
        std::string sig_i = SerializeOrder((*bucket)[i]->output_order) + "|p=" + ((*bucket)[i]->parameterized_like ? "1" : "0") +
                            "|m=" + ((*bucket)[i]->uses_memoize ? "1" : "0");
        for (std::size_t j = 0; j < bucket->size(); j++) {
            if (i == j) continue;
            std::string sig_j = SerializeOrder((*bucket)[j]->output_order) + "|p=" + ((*bucket)[j]->parameterized_like ? "1" : "0") +
                                "|m=" + ((*bucket)[j]->uses_memoize ? "1" : "0");
            if (sig_i != sig_j) continue;
            if ((*bucket)[j]->total_cost <= (*bucket)[i]->total_cost &&
                (*bucket)[j]->startup_cost <= (*bucket)[i]->startup_cost &&
                ((*bucket)[j]->total_cost < (*bucket)[i]->total_cost || (*bucket)[j]->startup_cost < (*bucket)[i]->startup_cost)) {
                dominated = true;
                break;
            }
        }
        if (!dominated) nondominated.push_back((*bucket)[i]);
    }

    auto objective = [&](const std::shared_ptr<PhysicalPath> &path) {
        CostModel::CostEstimate final_cost{path->startup_cost, path->total_cost};
        return cost_model_.BlendForLimit(final_cost, path->est_rows, query.limit_count);
    };

    std::vector<std::shared_ptr<PhysicalPath>> keep;
    auto keep_best = [&](auto pred, auto score_fn) {
        std::shared_ptr<PhysicalPath> best = nullptr;
        double best_score = 0.0;
        for (const auto &path : nondominated) {
            if (!pred(path)) continue;
            double score = score_fn(path);
            if (!best || score < best_score) {
                best = path;
                best_score = score;
            }
        }
        if (best != nullptr && std::find(keep.begin(), keep.end(), best) == keep.end()) keep.push_back(best);
    };

    keep_best([](const std::shared_ptr<PhysicalPath> &) { return true; }, objective);
    keep_best([](const std::shared_ptr<PhysicalPath> &) { return true; }, [](const std::shared_ptr<PhysicalPath> &p) { return p->startup_cost; });
    keep_best([](const std::shared_ptr<PhysicalPath> &p) { return p->parameterized_like; }, objective);
    keep_best([&](const std::shared_ptr<PhysicalPath> &p) { return p->SatisfiesOrder(query.required_order); }, objective);
    keep_best([&](const std::shared_ptr<PhysicalPath> &p) { return p->SatisfiesOrder(query.group_order); }, objective);

    std::unordered_map<std::string, std::shared_ptr<PhysicalPath>> best_by_order;
    for (const auto &path : nondominated) {
        if (path->output_order.empty()) continue;
        std::string sig = SerializeOrder(path->output_order);
        auto it = best_by_order.find(sig);
        if (it == best_by_order.end() || objective(path) < objective(it->second)) best_by_order[sig] = path;
    }
    for (const auto &[sig, path] : best_by_order) {
        (void)sig;
        if (std::find(keep.begin(), keep.end(), path) == keep.end()) keep.push_back(path);
    }

    *bucket = std::move(keep);
}

double PlanEnumerator::ComputeFinalCost(const std::shared_ptr<PhysicalPath> &path,
                                        const OptimizerQuery &query) const {
    CostModel::CostEstimate final_cost{path->startup_cost, path->total_cost};
    if (!query.has_aggregate && !query.required_order.empty() && !path->SatisfiesOrder(query.required_order)) {
        auto sort_cost = cost_model_.EstimateSortCost(path->est_rows);
        final_cost.startup_cost = path->total_cost + sort_cost.startup_cost;
        final_cost.total_cost = path->total_cost + sort_cost.total_cost;
    }
    if (query.has_aggregate && !query.group_order.empty()) {
        if (path->SatisfiesOrder(query.group_order)) {
            auto agg = cost_model_.EstimateStreamAggregateCost(path->est_rows);
            final_cost.startup_cost += agg.startup_cost;
            final_cost.total_cost += agg.total_cost;
        } else {
            auto hash = cost_model_.EstimateHashAggregateCost(path->est_rows);
            auto sorted = cost_model_.EstimateSortCost(path->est_rows);
            auto stream = cost_model_.EstimateStreamAggregateCost(path->est_rows);
            auto output_sort = (!query.required_order.empty()) ? cost_model_.EstimateSortCost(std::max(1.0, path->est_rows * 0.25))
                                                               : CostModel::CostEstimate{};
            double hash_total = path->total_cost + hash.total_cost + output_sort.total_cost;
            double sort_total = path->total_cost + sorted.total_cost + stream.total_cost;
            if (hash_total <= sort_total) {
                final_cost.startup_cost = path->total_cost + hash.startup_cost + output_sort.startup_cost;
                final_cost.total_cost = hash_total;
            } else {
                final_cost.startup_cost = path->total_cost + sorted.total_cost + stream.startup_cost;
                final_cost.total_cost = sort_total;
            }
        }
    }
    return cost_model_.BlendForLimit(final_cost, path->est_rows, query.limit_count);
}

std::shared_ptr<PhysicalPath> PlanEnumerator::OptimizeLogicalPlan(
    const LogicalPlanNode &logical_root,
    const std::vector<PlanOrderKey> &required_order,
    const std::vector<PlanOrderKey> &group_order,
    bool has_aggregate,
    std::optional<std::size_t> limit_count) const {
    OptimizerQuery query;
    CollectLogicalQuery(logical_root, &query.relations, &query.predicates, &query.joins);
    std::sort(query.relations.begin(), query.relations.end());
    query.relations.erase(std::unique(query.relations.begin(), query.relations.end()), query.relations.end());
    query.required_order = required_order;
    query.group_order = group_order;
    query.has_aggregate = has_aggregate;
    query.limit_count = limit_count;
    return Optimize(query);
}

std::shared_ptr<PhysicalPath> PlanEnumerator::Optimize(const OptimizerQuery &query) const {
    if (query.relations.empty()) throw std::runtime_error("Cannot optimize an empty query");
    if (query.relations.size() > 63) throw std::runtime_error("Optimizer supports at most 63 base relations");

    std::unordered_map<RelationId, std::size_t> rel_pos;
    for (std::size_t i = 0; i < query.relations.size(); i++) rel_pos[query.relations[i]] = i;

    std::unordered_map<uint64_t, std::vector<std::shared_ptr<PhysicalPath>>> best_for_subset;
    for (std::size_t i = 0; i < query.relations.size(); i++) {
        RelationId rel_id = query.relations[i];
        uint64_t bit = (1ULL << i);
        auto base_candidates = MakeBaseCandidates(rel_id, bit, GetPredicatesForRelation(query, rel_id), query);
        for (auto &candidate : base_candidates) AddPrunedCandidate(&best_for_subset[bit], candidate, query);
    }

    const std::size_t n = query.relations.size();
    for (std::size_t subset_size = 2; subset_size <= n; subset_size++) {
        for (uint64_t subset = 1; subset < (1ULL << n); subset++) {
            if (__builtin_popcountll(subset) != static_cast<int>(subset_size)) continue;
            std::vector<std::shared_ptr<PhysicalPath>> bucket;
            for (std::size_t rel_idx = 0; rel_idx < n; rel_idx++) {
                if (((subset >> rel_idx) & 1ULL) == 0) continue;
                uint64_t right_bit = (1ULL << rel_idx);
                uint64_t left_subset = subset & ~right_bit;
                if (left_subset == 0) continue;

                auto it_left = best_for_subset.find(left_subset);
                auto it_right = best_for_subset.find(right_bit);
                if (it_left == best_for_subset.end() || it_right == best_for_subset.end()) continue;

                RelationId right_rel = query.relations[rel_idx];
                auto joins = GetJoinsBetweenSubsetAndRelation(query, left_subset, right_rel, rel_pos);
                for (const auto &left_candidate : it_left->second) {
                    for (const auto &right_candidate : it_right->second) {
                        for (const auto &join_pred : joins) {
                            auto join_candidates = TryBuildJoinPaths(left_candidate, right_candidate, join_pred, query);
                            for (auto &candidate : join_candidates) AddPrunedCandidate(&bucket, std::move(candidate), query);
                        }
                    }
                }
            }
            if (!bucket.empty()) best_for_subset[subset] = std::move(bucket);
        }
    }

    uint64_t full_mask = (1ULL << n) - 1ULL;
    auto it = best_for_subset.find(full_mask);
    if (it == best_for_subset.end()) {
        throw std::runtime_error("Failed to build a complete plan. Query graph may be disconnected.");
    }

    std::shared_ptr<PhysicalPath> best = nullptr;
    double best_total_cost = 0.0;
    for (const auto &candidate : it->second) {
        double total_cost = ComputeFinalCost(candidate, query);
        if (!best || total_cost < best_total_cost) {
            best = candidate;
            best_total_cost = total_cost;
        }
    }
    return best;
}

}  // namespace simpledb
