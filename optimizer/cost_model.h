#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../catalog/stats_catalog.h"
#include "../execution/expressions.h"

namespace simpledb {

class CostModel {
public:
    struct CostEstimate {
        double startup_cost{0.0};
        double total_cost{0.0};
    };

    double seq_page_read_cost{1.0};
    double random_page_read_cost{4.0};
    double cpu_tuple_cost{0.01};
    double cpu_predicate_cost{0.002};
    double cpu_index_tuple_cost{0.005};
    double cpu_operator_cost{0.0025};
    double index_descent_cost{3.0};
    double effective_cache_size_pages{4096.0};
    std::size_t work_mem_bytes{1ULL << 20};
    double assumed_row_width_bytes{64.0};

    static double ClampSelectivity(double sel) {
        return std::clamp(sel, 0.000001, 1.0);
    }

    static bool ValuesEqualForStats(const Value &a, const Value &b) {
        if (a.IsNull() || b.IsNull()) return false;
        if (a.GetTypeId() != b.GetTypeId()) {
            if (IsNumericType(a.GetTypeId()) && IsNumericType(b.GetTypeId())) {
                return std::fabs(ValueToDouble(a) - ValueToDouble(b)) < 1e-12;
            }
            return false;
        }
        return CompareValues(a, b) == 0;
    }

    double EstimatePredicateSelectivity(const RelationStats &stats,
                                        std::size_t column_idx,
                                        ComparisonType cmp,
                                        const Value &constant) const {
        auto it = stats.columns.find(column_idx);
        if (it == stats.columns.end()) return 0.33;
        const ColumnStats &cs = it->second;
        if (stats.tuple_count == 0) return 0.0;

        auto eq_selectivity = [&]() {
            double remaining_mass = std::max(0.0, 1.0 - cs.null_fraction);
            for (const auto &mcv : cs.mcv_entries) {
                if (ValuesEqualForStats(mcv.value, constant)) return ClampSelectivity(mcv.frequency);
                remaining_mass = std::max(0.0, remaining_mass - mcv.frequency);
            }
            std::size_t non_mcv_distinct = cs.distinct_count > cs.mcv_entries.size()
                                               ? (cs.distinct_count - cs.mcv_entries.size())
                                               : 1;
            return ClampSelectivity(remaining_mass / static_cast<double>(non_mcv_distinct));
        };

        if (cmp == ComparisonType::EQ) return eq_selectivity();
        if (cmp == ComparisonType::NEQ) return ClampSelectivity(1.0 - eq_selectivity());

        if (!cs.histogram.empty() && IsNumericType(constant.GetTypeId())) {
            double c = ValueToDouble(constant);
            double prev_cum = 0.0;
            double prev_upper = cs.has_numeric_minmax ? ValueToDouble(cs.min_value) : ValueToDouble(cs.histogram.front().upper_bound);
            bool first = true;
            for (const auto &bucket : cs.histogram) {
                double upper = ValueToDouble(bucket.upper_bound);
                if (c <= upper) {
                    double width = std::max(upper - prev_upper, 1e-9);
                    double within = first ? ((c <= upper) ? std::clamp((c - prev_upper) / width, 0.0, 1.0) : 1.0)
                                          : std::clamp((c - prev_upper) / width, 0.0, 1.0);
                    double frac = prev_cum + (bucket.cumulative_fraction - prev_cum) * within;
                    if (cmp == ComparisonType::LT || cmp == ComparisonType::LTE) return ClampSelectivity(frac);
                    return ClampSelectivity(1.0 - frac);
                }
                prev_cum = bucket.cumulative_fraction;
                prev_upper = upper;
                first = false;
            }
            if (cmp == ComparisonType::LT || cmp == ComparisonType::LTE) return 1.0;
            return 0.000001;
        }

        if (cs.has_numeric_minmax && IsNumericType(constant.GetTypeId())) {
            double minv = ValueToDouble(cs.min_value);
            double maxv = ValueToDouble(cs.max_value);
            double c = ValueToDouble(constant);
            if (maxv <= minv + 1e-12) return 0.5;
            double frac = std::clamp((c - minv) / (maxv - minv), 0.0, 1.0);
            if (cmp == ComparisonType::LT || cmp == ComparisonType::LTE) return ClampSelectivity(frac);
            return ClampSelectivity(1.0 - frac);
        }
        return 0.33;
    }

    bool TryEstimatePairEqualitySelectivity(const RelationStats &stats,
                                            std::size_t first_col,
                                            const Value &first_const,
                                            std::size_t second_col,
                                            const Value &second_const,
                                            double *out) const {
        uint64_t key = EncodeMultiColumnStatsKey(first_col, second_col);
        auto it = stats.multi_column_stats.find(key);
        if (it == stats.multi_column_stats.end()) return false;

        const auto &pair_stats = it->second;
        bool flipped = pair_stats.first_column_idx != first_col;
        for (const auto &mcv : pair_stats.mcv_pairs) {
            const Value &a = flipped ? mcv.second_value : mcv.first_value;
            const Value &b = flipped ? mcv.first_value : mcv.second_value;
            if (ValuesEqualForStats(a, first_const) && ValuesEqualForStats(b, second_const)) {
                *out = ClampSelectivity(mcv.frequency);
                return true;
            }
        }

        if (pair_stats.joint_distinct_count == 0) return false;
        *out = ClampSelectivity((1.0 - ColumnMcvMass(stats, first_col)) *
                                (1.0 - ColumnMcvMass(stats, second_col)) /
                                static_cast<double>(pair_stats.joint_distinct_count));
        return true;
    }

    double EstimateJoinRows(const RelationStats &left_stats,
                            std::size_t left_col_idx,
                            double left_rows,
                            const RelationStats &right_stats,
                            std::size_t right_col_idx,
                            double right_rows) const {
        std::size_t left_distinct = 0;
        std::size_t right_distinct = 0;
        auto it_l = left_stats.columns.find(left_col_idx);
        if (it_l != left_stats.columns.end()) left_distinct = it_l->second.distinct_count;
        auto it_r = right_stats.columns.find(right_col_idx);
        if (it_r != right_stats.columns.end()) right_distinct = it_r->second.distinct_count;

        double base_frac = 0.1;
        if (left_distinct > 0 && right_distinct > 0) {
            base_frac = 1.0 / static_cast<double>(std::max(left_distinct, right_distinct));
        }

        double overlap_frac = 0.0;
        if (it_l != left_stats.columns.end() && it_r != right_stats.columns.end()) {
            std::unordered_map<std::string, double> right_mcv;
            double right_mcv_mass = 0.0;
            for (const auto &mcv : it_r->second.mcv_entries) {
                right_mcv[SerializeValueForHash(mcv.value)] = mcv.frequency;
                right_mcv_mass += mcv.frequency;
            }

            double left_mcv_mass = 0.0;
            for (const auto &mcv : it_l->second.mcv_entries) {
                left_mcv_mass += mcv.frequency;
                auto match = right_mcv.find(SerializeValueForHash(mcv.value));
                if (match != right_mcv.end()) overlap_frac += mcv.frequency * match->second;
            }

            std::size_t left_remaining_ndv = it_l->second.distinct_count > it_l->second.mcv_entries.size()
                                                 ? it_l->second.distinct_count - it_l->second.mcv_entries.size()
                                                 : 1;
            std::size_t right_remaining_ndv = it_r->second.distinct_count > it_r->second.mcv_entries.size()
                                                  ? it_r->second.distinct_count - it_r->second.mcv_entries.size()
                                                  : 1;
            std::size_t remaining_ndv = std::max<std::size_t>(1, std::max(left_remaining_ndv, right_remaining_ndv));
            double remaining_left_mass = std::max(0.0, 1.0 - it_l->second.null_fraction - left_mcv_mass);
            double remaining_right_mass = std::max(0.0, 1.0 - it_r->second.null_fraction - right_mcv_mass);
            overlap_frac += (remaining_left_mass * remaining_right_mass) / static_cast<double>(remaining_ndv);
        }

        double join_frac = std::max(base_frac, overlap_frac);
        return std::max(1.0, left_rows * right_rows * ClampSelectivity(join_frac));
    }

    CostEstimate EstimateSeqScanCost(const RelationStats &stats,
                                     double selectivity,
                                     std::size_t num_predicates) const {
        double rows = std::max(1.0, static_cast<double>(stats.tuple_count) * ClampSelectivity(selectivity));
        double total = static_cast<double>(stats.page_count) * seq_page_read_cost +
                       static_cast<double>(stats.tuple_count) * cpu_tuple_cost +
                       static_cast<double>(stats.tuple_count) * static_cast<double>(num_predicates) * cpu_predicate_cost;
        double startup = seq_page_read_cost + std::min(rows, 32.0) * (cpu_tuple_cost + num_predicates * cpu_predicate_cost);
        return {startup, total};
    }

    CostEstimate EstimateIndexScanCost(const RelationStats &stats,
                                       double selectivity,
                                       std::size_t num_residual_predicates,
                                       bool ordered_output = false) const {
        double rows = std::max(1.0, static_cast<double>(stats.tuple_count) * ClampSelectivity(selectivity));
        double pages = std::max(1.0, static_cast<double>(stats.page_count) * std::clamp(selectivity * 1.25, 0.0, 1.0));
        double cache_discount = std::clamp(effective_cache_size_pages / std::max(1.0, static_cast<double>(stats.page_count)), 0.0, 0.95);
        double effective_random_cost = random_page_read_cost * (1.0 - 0.5 * cache_discount);
        double startup = index_descent_cost * effective_random_cost + effective_random_cost + cpu_index_tuple_cost;
        double total = index_descent_cost * effective_random_cost +
                       pages * effective_random_cost +
                       rows * (cpu_index_tuple_cost + cpu_tuple_cost + num_residual_predicates * cpu_predicate_cost);
        if (ordered_output) total *= 0.95;
        return {startup, total};
    }

    CostEstimate EstimateNestedLoopJoinCost(double left_startup,
                                            double left_total,
                                            double right_startup,
                                            double right_total,
                                            double left_rows,
                                            double right_rows) const {
        double startup = left_startup + right_startup + cpu_operator_cost;
        double total = left_total + left_rows * right_total + left_rows * right_rows * cpu_operator_cost;
        return {startup, total};
    }

    CostEstimate EstimateHashJoinCost(double left_startup,
                                      double left_total,
                                      double right_startup,
                                      double right_total,
                                      double left_rows,
                                      double right_rows) const {
        double build_rows = std::min(left_rows, right_rows);
        double probe_rows = std::max(left_rows, right_rows);
        double build_bytes = build_rows * assumed_row_width_bytes;
        double spill_penalty = build_bytes > static_cast<double>(work_mem_bytes)
                                   ? ((build_bytes / static_cast<double>(work_mem_bytes)) - 1.0) * seq_page_read_cost * 4.0
                                   : 0.0;
        double startup = std::min(left_total, right_total) + build_rows * cpu_tuple_cost + spill_penalty;
        double total = left_total + right_total + build_rows * cpu_tuple_cost * 1.5 +
                       probe_rows * cpu_operator_cost + spill_penalty;
        return {startup, total};
    }

    CostEstimate EstimateMergeJoinCost(double left_startup,
                                       double left_total,
                                       double right_startup,
                                       double right_total,
                                       double left_rows,
                                       double right_rows) const {
        double startup = std::max(left_startup, right_startup) + cpu_operator_cost;
        double total = left_total + right_total + (left_rows + right_rows) * cpu_operator_cost * 0.75;
        return {startup, total};
    }

    CostEstimate EstimateIndexNestedLoopJoinCost(double left_startup,
                                                 double left_total,
                                                 double left_rows,
                                                 double inner_lookup_selectivity) const {
        double per_lookup = index_descent_cost * random_page_read_cost +
                            std::max(1.0, inner_lookup_selectivity * 4.0) * (random_page_read_cost + cpu_index_tuple_cost + cpu_tuple_cost);
        double startup = left_startup + per_lookup;
        double total = left_total + left_rows * (per_lookup + cpu_operator_cost);
        return {startup, total};
    }

    CostEstimate EstimateMemoizedIndexNestedLoopJoinCost(double left_startup,
                                                         double left_total,
                                                         double left_rows,
                                                         double distinct_outer_keys,
                                                         double inner_lookup_selectivity) const {
        double per_lookup = index_descent_cost * random_page_read_cost +
                            std::max(1.0, inner_lookup_selectivity * 4.0) * (random_page_read_cost + cpu_index_tuple_cost + cpu_tuple_cost);
        double cache_probe_cost = cpu_operator_cost * 0.5;
        double startup = left_startup + per_lookup;
        double total = left_total + distinct_outer_keys * per_lookup + left_rows * cache_probe_cost;
        return {startup, total};
    }

    CostEstimate EstimateSortCost(double rows) const {
        double n = std::max(2.0, rows);
        double total = n * std::log2(n) * cpu_operator_cost * 2.0;
        double bytes = n * assumed_row_width_bytes;
        if (bytes > static_cast<double>(work_mem_bytes)) {
            total += ((bytes / static_cast<double>(work_mem_bytes)) - 1.0) * seq_page_read_cost * 4.0;
        }
        return {total, total};
    }

    CostEstimate EstimateHashAggregateCost(double rows) const {
        double bytes = rows * assumed_row_width_bytes;
        double spill_penalty = bytes > static_cast<double>(work_mem_bytes)
                                   ? ((bytes / static_cast<double>(work_mem_bytes)) - 1.0) * seq_page_read_cost * 3.0
                                   : 0.0;
        double total = rows * cpu_operator_cost * 1.5 + spill_penalty;
        return {total, total};
    }

    CostEstimate EstimateStreamAggregateCost(double rows) const {
        double total = rows * cpu_operator_cost * 0.75;
        return {cpu_operator_cost, total};
    }

    double BlendForLimit(const CostEstimate &cost,
                         double est_rows,
                         const std::optional<std::size_t> &limit_count) const {
        if (!limit_count.has_value()) return cost.total_cost;
        double rows = std::max(1.0, est_rows);
        double needed = std::min(rows, static_cast<double>(*limit_count));
        double frac = std::clamp(needed / rows, 0.01, 1.0);
        return cost.startup_cost + frac * (cost.total_cost - cost.startup_cost);
    }

private:
    double ColumnMcvMass(const RelationStats &stats, std::size_t column_idx) const {
        auto it = stats.columns.find(column_idx);
        if (it == stats.columns.end()) return 0.0;
        double mass = 0.0;
        for (const auto &mcv : it->second.mcv_entries) mass += mcv.frequency;
        return mass;
    }
};

}  // namespace simpledb
