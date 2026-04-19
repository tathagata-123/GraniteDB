#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/types.h"
#include "../common/value.h"
#include "catalog_manager.h"

namespace simpledb {

struct HistogramBucket {
    Value upper_bound = Value::Null(TypeId::DOUBLE);
    double cumulative_fraction{0.0};
};

struct MCVEntry {
    Value value = Value::Null(TypeId::DOUBLE);
    double frequency{0.0};
};

struct PairMCVEntry {
    Value first_value = Value::Null(TypeId::DOUBLE);
    Value second_value = Value::Null(TypeId::DOUBLE);
    double frequency{0.0};
};

struct ColumnStats {
    std::size_t distinct_count{0};
    double null_fraction{0.0};
    bool has_numeric_minmax{false};
    Value min_value = Value::Null(TypeId::DOUBLE);
    Value max_value = Value::Null(TypeId::DOUBLE);
    std::vector<HistogramBucket> histogram;
    std::vector<MCVEntry> mcv_entries;
};

struct MultiColumnStats {
    std::size_t first_column_idx{0};
    std::size_t second_column_idx{0};
    std::size_t joint_distinct_count{0};
    std::vector<PairMCVEntry> mcv_pairs;
};

inline uint64_t EncodeMultiColumnStatsKey(std::size_t first_col, std::size_t second_col) {
    if (first_col > second_col) std::swap(first_col, second_col);
    return (static_cast<uint64_t>(first_col) << 32U) | static_cast<uint64_t>(second_col);
}

struct RelationStats {
    std::size_t tuple_count{0};
    std::size_t page_count{0};
    std::unordered_map<std::size_t, ColumnStats> columns;
    std::unordered_map<uint64_t, MultiColumnStats> multi_column_stats;
};

class StatsCatalog {
public:
    explicit StatsCatalog(std::string stats_file_path = "");

    void AnalyzeRelation(const CatalogManager &catalog, RelationId relation_id);
    const RelationStats &GetRelationStats(RelationId relation_id) const;
    bool HasRelationStats(RelationId relation_id) const;

    bool Save() const;
    bool Load();
    const std::string &GetStatsFilePath() const { return stats_file_path_; }
private:
    std::unordered_map<RelationId, RelationStats> relation_stats_;
    std::string stats_file_path_;
};

}  // namespace simpledb
