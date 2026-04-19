#pragma once

#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../catalog/catalog_manager.h"
#include "../execution/operators.h"

namespace simpledb {

enum class AccessPathType {
    SEQ_SCAN,
    INDEX_SCAN_EQ,
    INDEX_SCAN_RANGE,
    INDEX_SCAN_ORDERED
};

enum class JoinAlgorithmType {
    NESTED_LOOP,
    INDEX_NESTED_LOOP,
    HASH_JOIN,
    MERGE_JOIN
};

struct PlanOrderKey {
    RelationId relation_id{0};
    std::size_t column_idx{0};
    bool ascending{true};
};

struct QueryPredicateSpec {
    RelationId relation_id{0};
    std::size_t column_idx{0};
    ComparisonType cmp{ComparisonType::EQ};
    Value constant = Value(int32_t(0));

    bool IsEquality() const { return cmp == ComparisonType::EQ; }
    bool IsRange() const {
        return cmp == ComparisonType::LT || cmp == ComparisonType::LTE ||
               cmp == ComparisonType::GT || cmp == ComparisonType::GTE;
    }
};

struct QueryJoinPredicateSpec {
    RelationId left_relation_id{0};
    std::size_t left_column_idx{0};
    RelationId right_relation_id{0};
    std::size_t right_column_idx{0};
};

struct IndexAccessSpec {
    std::vector<QueryPredicateSpec> index_predicates;
    std::vector<QueryPredicateSpec> residual_predicates;
    std::size_t equality_prefix_count{0};
    std::optional<QueryPredicateSpec> lower_bound;
    std::optional<QueryPredicateSpec> upper_bound;

    bool IsExactLookup() const {
        return !index_predicates.empty() && !lower_bound.has_value() && !upper_bound.has_value() &&
               equality_prefix_count == index_predicates.size();
    }
};

struct PhysicalPath {
    enum class Kind { BASE, JOIN };

    Kind kind{Kind::BASE};
    double cost{0.0};
    double startup_cost{0.0};
    double total_cost{0.0};
    double est_rows{0.0};
    uint64_t relation_mask{0};
    Schema output_schema;
    std::unordered_map<RelationId, std::size_t> relation_offsets;

    RelationId relation_id{0};
    AccessPathType access_type{AccessPathType::SEQ_SCAN};
    const IndexCatalogEntry *access_index{nullptr};
    std::vector<QueryPredicateSpec> all_predicates;
    IndexAccessSpec access_spec;

    JoinAlgorithmType join_type{JoinAlgorithmType::NESTED_LOOP};
    QueryJoinPredicateSpec join_predicate;
    bool merge_left_needs_sort{false};
    bool merge_right_needs_sort{false};
    bool uses_memoize{false};
    bool parameterized_like{false};
    std::shared_ptr<PhysicalPath> left;
    std::shared_ptr<PhysicalPath> right;

    std::vector<PlanOrderKey> output_order;
    std::vector<PlanOrderKey> fixed_order_prefix;

    static std::unique_ptr<AbstractExpression> BuildSingleTuplePredicateExpr(
        const std::vector<QueryPredicateSpec> &predicates) {
        if (predicates.empty()) return nullptr;
        std::unique_ptr<AbstractExpression> expr;
        for (const auto &p : predicates) {
            auto cmp_expr = std::make_unique<ComparisonExpression>(
                p.cmp,
                std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, p.column_idx),
                std::make_unique<ConstantValueExpression>(p.constant));
            if (!expr) expr = std::move(cmp_expr);
            else expr = std::make_unique<ConjunctionExpression>(std::move(expr), std::move(cmp_expr));
        }
        return expr;
    }

    static std::unique_ptr<AbstractExpression> BuildJoinedTuplePredicateExpr(
        const std::vector<QueryPredicateSpec> &predicates,
        const std::unordered_map<RelationId, std::size_t> &relation_offsets) {
        if (predicates.empty()) return nullptr;
        std::unique_ptr<AbstractExpression> expr;
        for (const auto &p : predicates) {
            auto it = relation_offsets.find(p.relation_id);
            if (it == relation_offsets.end()) {
                throw std::runtime_error("Missing relation offset while building joined filter");
            }
            std::size_t absolute_col = it->second + p.column_idx;
            auto cmp_expr = std::make_unique<ComparisonExpression>(
                p.cmp,
                std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, absolute_col),
                std::make_unique<ConstantValueExpression>(p.constant));
            if (!expr) expr = std::move(cmp_expr);
            else expr = std::make_unique<ConjunctionExpression>(std::move(expr), std::move(cmp_expr));
        }
        return expr;
    }

    bool SatisfiesOrder(const std::vector<PlanOrderKey> &required_order) const {
        if (required_order.empty()) return true;
        std::size_t out_pos = 0;
        for (const auto &need : required_order) {
            if (IsFixedOrderKey(need)) {
                continue;
            }
            while (out_pos < output_order.size() && IsFixedOrderKey(output_order[out_pos])) {
                out_pos++;
            }
            if (out_pos >= output_order.size()) return false;
            const auto &have = output_order[out_pos];
            if (have.relation_id != need.relation_id || have.column_idx != need.column_idx || have.ascending != need.ascending) {
                return false;
            }
            out_pos++;
        }
        return true;
    }

    std::vector<RID> CollectBaseRids(const CatalogManager &catalog) const {
        if (kind != Kind::BASE) {
            throw std::runtime_error("CollectBaseRids can only be used on base access paths");
        }
        const auto &rel = catalog.GetRelation(relation_id);
        std::vector<RID> rids;
        if (access_type == AccessPathType::SEQ_SCAN || access_index == nullptr) {
            HeapFileIterator it(rel.heap_file);
            while (it.HasNext()) {
                auto [rid, tuple] = it.Next();
                (void)tuple;
                rids.push_back(rid);
            }
            return rids;
        }
        if (access_index->GetBTreeIndex() != nullptr) {
            const BTreeIndex *btree = access_index->GetBTreeIndex();
            if (access_type == AccessPathType::INDEX_SCAN_EQ && !access_spec.index_predicates.empty()) {
                return btree->Search(access_spec.index_predicates.front().constant);
            }
            std::unique_ptr<BTreeIndexIterator> it;
            if (access_type == AccessPathType::INDEX_SCAN_RANGE && access_spec.lower_bound.has_value()) {
                it = std::make_unique<BTreeIndexIterator>(btree, access_spec.lower_bound->constant);
            } else {
                it = std::make_unique<BTreeIndexIterator>(btree);
            }
            while (it->HasNext()) {
                auto [key, rid] = it->Next();
                if (access_type == AccessPathType::INDEX_SCAN_RANGE && access_spec.lower_bound.has_value()) {
                    int cmp = CompareValues(key, access_spec.lower_bound->constant);
                    bool lower_inclusive = access_spec.lower_bound->cmp == ComparisonType::GTE;
                    if (cmp < 0 || (cmp == 0 && !lower_inclusive)) continue;
                }
                if (access_spec.upper_bound.has_value()) {
                    int cmp = CompareValues(key, access_spec.upper_bound->constant);
                    bool upper_inclusive = access_spec.upper_bound->cmp == ComparisonType::LTE;
                    if (cmp > 0 || (cmp == 0 && !upper_inclusive)) break;
                }
                rids.push_back(rid);
            }
            return rids;
        }
        const GenericBTreeIndex *generic = access_index->GetGenericBTreeIndex();
        if (generic == nullptr) throw std::runtime_error("Missing runtime generic index");
        GenericBTreeIndex::PrefixScanSpec scan_spec;
        for (std::size_t i = 0; i < access_spec.equality_prefix_count && i < access_spec.index_predicates.size(); i++) {
            scan_spec.equality_prefix.push_back(access_spec.index_predicates[i].constant);
        }
        if (access_spec.lower_bound.has_value()) {
            scan_spec.lower_bound = access_spec.lower_bound->constant;
            scan_spec.lower_inclusive = (access_spec.lower_bound->cmp == ComparisonType::GTE);
        }
        if (access_spec.upper_bound.has_value()) {
            scan_spec.upper_bound = access_spec.upper_bound->constant;
            scan_spec.upper_inclusive = (access_spec.upper_bound->cmp == ComparisonType::LTE);
        }
        bool full_scan = access_type == AccessPathType::INDEX_SCAN_ORDERED &&
                         scan_spec.equality_prefix.empty() &&
                         !scan_spec.lower_bound.has_value() &&
                         !scan_spec.upper_bound.has_value();
        return full_scan ? generic->FullScanRids() : generic->ScanPrefixRange(scan_spec);
    }

    std::unique_ptr<AbstractExecutor> BuildExecutor(const CatalogManager &catalog,
                                                    TransactionPtr txn = nullptr,
                                                    LockManager *lock_manager = nullptr) const {
        if (kind == Kind::BASE) {
            const auto &rel = catalog.GetRelation(relation_id);
            std::unique_ptr<AbstractExecutor> exec;

            if (access_type == AccessPathType::SEQ_SCAN || access_index == nullptr) {
                exec = std::make_unique<SeqScanExecutor>(rel.heap_file, txn, lock_manager);
            } else if (access_index->GetBTreeIndex() != nullptr) {
                const BTreeIndex *btree = access_index->GetBTreeIndex();
                if (access_type == AccessPathType::INDEX_SCAN_EQ && !access_spec.index_predicates.empty()) {
                    exec = std::make_unique<IndexScanExecutor>(
                        rel.heap_file, btree, access_spec.index_predicates.front().constant, txn, lock_manager);
                } else if (access_type == AccessPathType::INDEX_SCAN_RANGE &&
                           (access_spec.lower_bound.has_value() || access_spec.upper_bound.has_value())) {
                    std::optional<Value> lower;
                    std::optional<Value> upper;
                    bool lower_inclusive = true;
                    bool upper_inclusive = true;
                    if (access_spec.lower_bound.has_value()) {
                        lower = access_spec.lower_bound->constant;
                        lower_inclusive = (access_spec.lower_bound->cmp == ComparisonType::GTE);
                    }
                    if (access_spec.upper_bound.has_value()) {
                        upper = access_spec.upper_bound->constant;
                        upper_inclusive = (access_spec.upper_bound->cmp == ComparisonType::LTE);
                    }
                    exec = std::make_unique<IndexScanExecutor>(
                        rel.heap_file, btree, lower, upper, lower_inclusive, upper_inclusive, txn, lock_manager);
                } else {
                    exec = std::make_unique<IndexScanExecutor>(rel.heap_file, btree, txn, lock_manager);
                }
            } else {
                const GenericBTreeIndex *generic = access_index->GetGenericBTreeIndex();
                if (generic == nullptr) {
                    throw std::runtime_error("Composite index path selected without runtime generic index");
                }
                GenericBTreeIndex::PrefixScanSpec scan_spec;
                for (std::size_t i = 0; i < access_spec.equality_prefix_count && i < access_spec.index_predicates.size(); i++) {
                    scan_spec.equality_prefix.push_back(access_spec.index_predicates[i].constant);
                }
                if (access_spec.lower_bound.has_value()) {
                    scan_spec.lower_bound = access_spec.lower_bound->constant;
                    scan_spec.lower_inclusive = (access_spec.lower_bound->cmp == ComparisonType::GTE);
                }
                if (access_spec.upper_bound.has_value()) {
                    scan_spec.upper_bound = access_spec.upper_bound->constant;
                    scan_spec.upper_inclusive = (access_spec.upper_bound->cmp == ComparisonType::LTE);
                }
                bool full_scan = access_type == AccessPathType::INDEX_SCAN_ORDERED &&
                                 scan_spec.equality_prefix.empty() &&
                                 !scan_spec.lower_bound.has_value() &&
                                 !scan_spec.upper_bound.has_value();
                exec = std::make_unique<GenericIndexScanExecutor>(
                    rel.heap_file, generic, std::move(scan_spec), full_scan, txn, lock_manager);
            }

            auto residual = BuildSingleTuplePredicateExpr(access_spec.residual_predicates);
            if (residual) exec = std::make_unique<FilterExecutor>(std::move(exec), std::move(residual));
            return exec;
        }

        auto left_exec = left->BuildExecutor(catalog, txn, lock_manager);
        auto right_exec = right->BuildExecutor(catalog, txn, lock_manager);

        bool left_has_left_rel = left->relation_offsets.count(join_predicate.left_relation_id) != 0;
        bool right_has_left_rel = right->relation_offsets.count(join_predicate.left_relation_id) != 0;
        RelationId left_rel_in_tree = join_predicate.left_relation_id;
        std::size_t left_col_in_rel = join_predicate.left_column_idx;
        RelationId right_rel_in_tree = join_predicate.right_relation_id;
        std::size_t right_col_in_rel = join_predicate.right_column_idx;
        if (!left_has_left_rel && right_has_left_rel) {
            left_rel_in_tree = join_predicate.right_relation_id;
            left_col_in_rel = join_predicate.right_column_idx;
            right_rel_in_tree = join_predicate.left_relation_id;
            right_col_in_rel = join_predicate.left_column_idx;
        }

        std::size_t left_col = left->relation_offsets.at(left_rel_in_tree) + left_col_in_rel;
        std::size_t right_col = right->relation_offsets.at(right_rel_in_tree) + right_col_in_rel;

        if (join_type == JoinAlgorithmType::NESTED_LOOP) {
            auto pred = std::make_unique<ComparisonExpression>(
                ComparisonType::EQ,
                std::make_unique<ColumnValueExpression>(TupleSide::LEFT, left_col),
                std::make_unique<ColumnValueExpression>(TupleSide::RIGHT, right_col));
            return std::make_unique<NestedLoopJoinExecutor>(
                std::move(left_exec), std::move(right_exec), std::move(pred));
        }

        if (join_type == JoinAlgorithmType::HASH_JOIN) {
            auto left_key = std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, left_col);
            auto right_key = std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, right_col);
            bool build_left_side = left->est_rows <= right->est_rows;
            return std::make_unique<HashJoinExecutor>(
                std::move(left_exec), std::move(right_exec), std::move(left_key), std::move(right_key), build_left_side);
        }

        if (join_type == JoinAlgorithmType::MERGE_JOIN) {
            if (merge_left_needs_sort) {
                std::vector<SortKeySpec> sort_keys;
                SortKeySpec spec;
                spec.expr = std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, left_col);
                spec.ascending = true;
                sort_keys.push_back(std::move(spec));
                left_exec = std::make_unique<SortExecutor>(std::move(left_exec), std::move(sort_keys));
            }
            if (merge_right_needs_sort) {
                std::vector<SortKeySpec> sort_keys;
                SortKeySpec spec;
                spec.expr = std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, right_col);
                spec.ascending = true;
                sort_keys.push_back(std::move(spec));
                right_exec = std::make_unique<SortExecutor>(std::move(right_exec), std::move(sort_keys));
            }
            if (!right_exec->SupportsMarkRestore()) {
                right_exec = std::make_unique<MaterializeExecutor>(std::move(right_exec));
            }
            auto left_key = std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, left_col);
            auto right_key = std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, right_col);
            return std::make_unique<MergeJoinExecutor>(
                std::move(left_exec), std::move(right_exec), std::move(left_key), std::move(right_key));
        }

        if (right->kind != Kind::BASE) {
            throw std::runtime_error("IndexNestedLoopJoin expects right child to be a base relation");
        }
        const auto &right_rel = catalog.GetRelation(right->relation_id);
        const IndexCatalogEntry *idx = catalog.FindIndexOnColumn(right_rel.relation_id, right_col_in_rel);
        if (idx == nullptr || idx->GetBTreeIndex() == nullptr) {
            throw std::runtime_error("IndexNestedLoopJoin selected without single-column B+ tree on inner relation");
        }
        auto left_key = std::make_unique<ColumnValueExpression>(TupleSide::SINGLE, left_col);
        std::unique_ptr<AbstractExecutor> joined;
        if (uses_memoize) {
            joined = std::make_unique<MemoizedIndexNestedLoopJoinExecutor>(
                std::move(left_exec), right_rel.heap_file, idx->GetBTreeIndex(), std::move(left_key), txn, lock_manager);
        } else {
            joined = std::make_unique<IndexNestedLoopJoinExecutor>(
                std::move(left_exec), right_rel.heap_file, idx->GetBTreeIndex(), std::move(left_key), txn, lock_manager);
        }
        if (!right->access_spec.residual_predicates.empty()) {
            auto joined_filter = BuildJoinedTuplePredicateExpr(right->access_spec.residual_predicates, relation_offsets);
            if (joined_filter) joined = std::make_unique<FilterExecutor>(std::move(joined), std::move(joined_filter));
        }
        return joined;
    }

    std::string Explain(int indent = 0) const {
        auto comparison_name = [](ComparisonType cmp) -> const char * {
            switch (cmp) {
                case ComparisonType::EQ: return "=";
                case ComparisonType::NEQ: return "!=";
                case ComparisonType::LT: return "<";
                case ComparisonType::LTE: return "<=";
                case ComparisonType::GT: return ">";
                case ComparisonType::GTE: return ">=";
            }
            return "?";
        };

        auto format_predicate = [&](const QueryPredicateSpec &pred) {
            std::ostringstream pred_out;
            pred_out << "c" << pred.column_idx << ' ' << comparison_name(pred.cmp) << ' ' << pred.constant.ToString();
            return pred_out.str();
        };

        auto format_order = [](const std::vector<PlanOrderKey> &order_keys) {
            std::ostringstream order_out;
            order_out << '[';
            for (std::size_t i = 0; i < order_keys.size(); i++) {
                if (i > 0) order_out << ", ";
                order_out << order_keys[i].relation_id << ".c" << order_keys[i].column_idx
                          << (order_keys[i].ascending ? " ASC" : " DESC");
            }
            order_out << ']';
            return order_out.str();
        };

        std::ostringstream out;
        std::string pad(indent, ' ');
        if (kind == Kind::BASE) {
            out << pad << "BaseAccess\n";
            out << pad << "  relation_id: " << relation_id << "\n";
            out << pad << "  est_rows: " << est_rows << "\n";
            out << pad << "  startup_cost: " << startup_cost << "\n";
            out << pad << "  total_cost: " << total_cost << "\n";
            out << pad << "  access: ";
            switch (access_type) {
                case AccessPathType::SEQ_SCAN: out << "SeqScan"; break;
                case AccessPathType::INDEX_SCAN_EQ: out << "IndexScan(EQ)"; break;
                case AccessPathType::INDEX_SCAN_RANGE: out << "IndexScan(RANGE)"; break;
                case AccessPathType::INDEX_SCAN_ORDERED: out << "IndexScan(ORDERED)"; break;
            }
            out << "\n";
            if (access_index != nullptr) {
                out << pad << "  index: " << access_index->index_name << "\n";
            }
            if (access_spec.equality_prefix_count > 0) {
                out << pad << "  equality_prefix_count: " << access_spec.equality_prefix_count << "\n";
            }
            if (!access_spec.index_predicates.empty()) {
                out << pad << "  index_conditions:\n";
                for (const auto &pred : access_spec.index_predicates) {
                    out << pad << "    - " << format_predicate(pred) << "\n";
                }
            }
            if (access_spec.lower_bound.has_value()) {
                out << pad << "  lower_bound: " << format_predicate(*access_spec.lower_bound) << "\n";
            }
            if (access_spec.upper_bound.has_value()) {
                out << pad << "  upper_bound: " << format_predicate(*access_spec.upper_bound) << "\n";
            }
            if (!access_spec.residual_predicates.empty()) {
                out << pad << "  residual_filters:\n";
                for (const auto &pred : access_spec.residual_predicates) {
                    out << pad << "    - " << format_predicate(pred) << "\n";
                }
            }
            if (!output_order.empty()) {
                out << pad << "  output_order: " << format_order(output_order) << "\n";
            }
            if (!fixed_order_prefix.empty()) {
                out << pad << "  fixed_prefix: " << format_order(fixed_order_prefix) << "\n";
            }
            return out.str();
        }

        out << pad << "Join\n";
        out << pad << "  est_rows: " << est_rows << "\n";
        out << pad << "  startup_cost: " << startup_cost << "\n";
        out << pad << "  total_cost: " << total_cost << "\n";
        out << pad << "  algorithm: ";
        switch (join_type) {
            case JoinAlgorithmType::NESTED_LOOP: out << "NestedLoop"; break;
            case JoinAlgorithmType::INDEX_NESTED_LOOP: out << (uses_memoize ? "Memoize + IndexNestedLoop" : "IndexNestedLoop"); break;
            case JoinAlgorithmType::HASH_JOIN: out << "HashJoin"; break;
            case JoinAlgorithmType::MERGE_JOIN: out << "MergeJoin"; break;
        }
        out << "\n";
        out << pad << "  predicate: "
            << join_predicate.left_relation_id << ".c" << join_predicate.left_column_idx
            << " = "
            << join_predicate.right_relation_id << ".c" << join_predicate.right_column_idx
            << "\n";
        if (!output_order.empty()) {
            out << pad << "  output_order: " << format_order(output_order) << "\n";
        }
        out << pad << "  left:\n" << left->Explain(indent + 4);
        out << pad << "  right:\n" << right->Explain(indent + 4);
        return out.str();
    }

private:
    bool IsFixedOrderKey(const PlanOrderKey &key) const {
        for (const auto &fixed : fixed_order_prefix) {
            if (fixed.relation_id == key.relation_id && fixed.column_idx == key.column_idx) {
                return true;
            }
        }
        return false;
    }
};

}  // namespace simpledb
