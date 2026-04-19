#include "stats_catalog.h"

#include <algorithm>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../execution/operators.h"

namespace simpledb {
namespace {

constexpr std::size_t kHistogramBuckets = 10;
constexpr std::size_t kMcvEntries = 8;
constexpr std::size_t kReservoirSampleSize = 1024;

std::string Escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '|') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::vector<std::string> SplitEscaped(const std::string &line) {
    std::vector<std::string> parts;
    std::string cur;
    bool esc = false;
    for (char c : line) {
        if (esc) {
            cur.push_back(c);
            esc = false;
            continue;
        }
        if (c == '\\') {
            esc = true;
            continue;
        }
        if (c == '|') {
            parts.push_back(cur);
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    parts.push_back(cur);
    return parts;
}

std::string SerializeValueText(const Value &v) {
    if (v.IsNull()) return "NULL";
    return v.ToString();
}

Value ParseValue(TypeId type, const std::string &s) {
    if (s == "NULL") return Value::Null(type);
    switch (type) {
        case TypeId::BOOLEAN: return Value(s == "true" || s == "1");
        case TypeId::INT32: return Value(static_cast<int32_t>(std::stoi(s)));
        case TypeId::INT64: return Value(static_cast<int64_t>(std::stoll(s)));
        case TypeId::DOUBLE: return Value(std::stod(s));
        case TypeId::VARCHAR: return Value(s);
        default: return Value::Null(type);
    }
}

void ReservoirAppend(std::vector<double> *samples,
                     std::size_t *seen,
                     double value,
                     std::mt19937_64 *rng) {
    (*seen)++;
    if (samples->size() < kReservoirSampleSize) {
        samples->push_back(value);
        return;
    }
    std::uniform_int_distribution<std::size_t> dist(0, *seen - 1);
    std::size_t pos = dist(*rng);
    if (pos < samples->size()) (*samples)[pos] = value;
}

std::vector<HistogramBucket> BuildHistogramFromSamples(std::vector<double> samples) {
    if (samples.empty()) return {};
    std::sort(samples.begin(), samples.end());
    std::vector<HistogramBucket> buckets;
    std::size_t bucket_count = std::min<std::size_t>(kHistogramBuckets, samples.size());
    buckets.reserve(bucket_count);
    for (std::size_t i = 1; i <= bucket_count; i++) {
        std::size_t idx = (i * samples.size() + bucket_count - 1) / bucket_count;
        idx = std::min<std::size_t>(samples.size() - 1, std::max<std::size_t>(1, idx) - 1);
        HistogramBucket bucket;
        bucket.upper_bound = Value(samples[idx]);
        bucket.cumulative_fraction = static_cast<double>(idx + 1) / static_cast<double>(samples.size());
        buckets.push_back(bucket);
    }
    if (!buckets.empty()) buckets.back().cumulative_fraction = 1.0;
    return buckets;
}

template <typename Entry>
void KeepTopByFrequency(std::vector<Entry> *entries, std::size_t max_entries) {
    std::sort(entries->begin(), entries->end(), [](const Entry &a, const Entry &b) {
        if (a.frequency != b.frequency) return a.frequency > b.frequency;
        return SerializeValueForHash(a.value) < SerializeValueForHash(b.value);
    });
    if (entries->size() > max_entries) entries->resize(max_entries);
}

void KeepTopPairsByFrequency(std::vector<PairMCVEntry> *entries, std::size_t max_entries) {
    std::sort(entries->begin(), entries->end(), [](const PairMCVEntry &a, const PairMCVEntry &b) {
        if (a.frequency != b.frequency) return a.frequency > b.frequency;
        std::string ak = SerializeValueForHash(a.first_value) + "|" + SerializeValueForHash(a.second_value);
        std::string bk = SerializeValueForHash(b.first_value) + "|" + SerializeValueForHash(b.second_value);
        return ak < bk;
    });
    if (entries->size() > max_entries) entries->resize(max_entries);
}

}  // namespace

StatsCatalog::StatsCatalog(std::string stats_file_path)
    : stats_file_path_(std::move(stats_file_path)) {}

void StatsCatalog::AnalyzeRelation(const CatalogManager &catalog, RelationId relation_id) {
    const auto &rel = catalog.GetRelation(relation_id);
    RelationStats stats;
    stats.page_count = rel.heap_file->GetNumPages();

    const std::size_t col_count = rel.schema.GetColumnCount();
    std::vector<std::unordered_set<std::string>> distinct_sets(col_count);
    std::vector<std::unordered_map<std::string, std::size_t>> freq_maps(col_count);
    std::vector<std::unordered_map<std::string, Value>> value_lookup(col_count);
    std::vector<std::size_t> null_counts(col_count, 0);
    std::vector<bool> minmax_init(col_count, false);
    std::vector<Value> min_vals(col_count, Value::Null(TypeId::DOUBLE));
    std::vector<Value> max_vals(col_count, Value::Null(TypeId::DOUBLE));
    std::vector<std::vector<double>> numeric_samples(col_count);
    std::vector<std::size_t> numeric_seen(col_count, 0);

    std::unordered_map<uint64_t, std::unordered_set<std::string>> pair_distinct_sets;
    std::unordered_map<uint64_t, std::unordered_map<std::string, std::size_t>> pair_freq_maps;
    std::unordered_map<uint64_t, std::unordered_map<std::string, std::pair<Value, Value>>> pair_value_lookup;

    std::mt19937_64 rng(0xC0FFEEULL + relation_id);

    SeqScanExecutor scan(rel.heap_file);
    scan.Init();
    Tuple tuple;
    while (scan.Next(&tuple)) {
        stats.tuple_count++;
        for (std::size_t i = 0; i < col_count; i++) {
            const Value &v = tuple.GetValue(i);
            if (v.IsNull()) {
                null_counts[i]++;
                continue;
            }
            std::string key = SerializeValueForHash(v);
            distinct_sets[i].insert(key);
            freq_maps[i][key]++;
            value_lookup[i][key] = v;

            TypeId t = v.GetTypeId();
            if (t == TypeId::INT32 || t == TypeId::INT64 || t == TypeId::DOUBLE) {
                if (!minmax_init[i]) {
                    minmax_init[i] = true;
                    min_vals[i] = v;
                    max_vals[i] = v;
                } else {
                    if (CompareValues(v, min_vals[i]) < 0) min_vals[i] = v;
                    if (CompareValues(v, max_vals[i]) > 0) max_vals[i] = v;
                }
                ReservoirAppend(&numeric_samples[i], &numeric_seen[i], ValueToDouble(v), &rng);
            }
        }

        for (std::size_t i = 0; i < col_count; i++) {
            const Value &vi = tuple.GetValue(i);
            if (vi.IsNull()) continue;
            for (std::size_t j = i + 1; j < col_count; j++) {
                const Value &vj = tuple.GetValue(j);
                if (vj.IsNull()) continue;
                uint64_t pair_key = EncodeMultiColumnStatsKey(i, j);
                std::string pair_value_key = SerializeValueForHash(vi) + "\x1f" + SerializeValueForHash(vj);
                pair_distinct_sets[pair_key].insert(pair_value_key);
                pair_freq_maps[pair_key][pair_value_key]++;
                pair_value_lookup[pair_key][pair_value_key] = {vi, vj};
            }
        }
    }
    scan.Close();

    for (std::size_t i = 0; i < col_count; i++) {
        ColumnStats cs;
        cs.distinct_count = distinct_sets[i].size();
        cs.null_fraction = stats.tuple_count == 0 ? 0.0 : static_cast<double>(null_counts[i]) / static_cast<double>(stats.tuple_count);
        if (minmax_init[i]) {
            cs.has_numeric_minmax = true;
            cs.min_value = min_vals[i];
            cs.max_value = max_vals[i];
            cs.histogram = BuildHistogramFromSamples(numeric_samples[i]);
        }

        std::vector<MCVEntry> mcvs;
        mcvs.reserve(freq_maps[i].size());
        for (const auto &[encoded, count] : freq_maps[i]) {
            MCVEntry entry;
            entry.value = value_lookup[i][encoded];
            entry.frequency = stats.tuple_count == 0 ? 0.0 : static_cast<double>(count) / static_cast<double>(stats.tuple_count);
            mcvs.push_back(entry);
        }
        KeepTopByFrequency(&mcvs, kMcvEntries);
        cs.mcv_entries = std::move(mcvs);
        stats.columns[i] = std::move(cs);
    }

    for (const auto &[pair_key, distincts] : pair_distinct_sets) {
        MultiColumnStats ms;
        ms.first_column_idx = static_cast<std::size_t>(pair_key >> 32U);
        ms.second_column_idx = static_cast<std::size_t>(pair_key & 0xffffffffULL);
        ms.joint_distinct_count = distincts.size();
        std::vector<PairMCVEntry> pair_mcvs;
        auto pf_it = pair_freq_maps.find(pair_key);
        if (pf_it != pair_freq_maps.end()) {
            pair_mcvs.reserve(pf_it->second.size());
            for (const auto &[encoded_pair, count] : pf_it->second) {
                const auto &vals = pair_value_lookup[pair_key][encoded_pair];
                PairMCVEntry entry;
                entry.first_value = vals.first;
                entry.second_value = vals.second;
                entry.frequency = stats.tuple_count == 0 ? 0.0 : static_cast<double>(count) / static_cast<double>(stats.tuple_count);
                pair_mcvs.push_back(entry);
            }
            KeepTopPairsByFrequency(&pair_mcvs, kMcvEntries);
        }
        ms.mcv_pairs = std::move(pair_mcvs);
        stats.multi_column_stats[pair_key] = std::move(ms);
    }

    relation_stats_[relation_id] = std::move(stats);
    Save();
}

const RelationStats &StatsCatalog::GetRelationStats(RelationId relation_id) const {
    auto it = relation_stats_.find(relation_id);
    if (it == relation_stats_.end()) throw std::runtime_error("Relation statistics not found");
    return it->second;
}

bool StatsCatalog::HasRelationStats(RelationId relation_id) const {
    return relation_stats_.count(relation_id) != 0;
}

bool StatsCatalog::Save() const {
    if (stats_file_path_.empty()) return false;
    std::ofstream out(stats_file_path_, std::ios::trunc);
    if (!out.is_open()) return false;

    out << "STATS_V2\n";
    out << relation_stats_.size() << "\n";
    for (const auto &[rid, stats] : relation_stats_) {
        out << "REL|" << rid << "|" << stats.tuple_count << "|" << stats.page_count << "|"
            << stats.columns.size() << "|" << stats.multi_column_stats.size() << "\n";
        for (const auto &[col_idx, cs] : stats.columns) {
            out << "COL|" << col_idx << "|" << cs.distinct_count << "|" << cs.null_fraction << "|"
                << (cs.has_numeric_minmax ? 1 : 0) << "|" << static_cast<int>(cs.min_value.GetTypeId()) << "|"
                << Escape(SerializeValueText(cs.min_value)) << "|" << Escape(SerializeValueText(cs.max_value)) << "|"
                << cs.histogram.size() << "|" << cs.mcv_entries.size() << "\n";
            for (const auto &bucket : cs.histogram) {
                out << "HIST|" << static_cast<int>(bucket.upper_bound.GetTypeId()) << "|"
                    << Escape(SerializeValueText(bucket.upper_bound)) << "|" << bucket.cumulative_fraction << "\n";
            }
            for (const auto &mcv : cs.mcv_entries) {
                out << "MCV|" << static_cast<int>(mcv.value.GetTypeId()) << "|"
                    << Escape(SerializeValueText(mcv.value)) << "|" << mcv.frequency << "\n";
            }
        }
        for (const auto &[key, ms] : stats.multi_column_stats) {
            out << "PAIR|" << ms.first_column_idx << "|" << ms.second_column_idx << "|"
                << ms.joint_distinct_count << "|" << ms.mcv_pairs.size() << "\n";
            for (const auto &mcv : ms.mcv_pairs) {
                out << "PAIRMCV|" << static_cast<int>(mcv.first_value.GetTypeId()) << "|"
                    << Escape(SerializeValueText(mcv.first_value)) << "|"
                    << static_cast<int>(mcv.second_value.GetTypeId()) << "|"
                    << Escape(SerializeValueText(mcv.second_value)) << "|"
                    << mcv.frequency << "\n";
            }
        }
    }
    return true;
}

bool StatsCatalog::Load() {
    if (stats_file_path_.empty()) return false;
    std::ifstream in(stats_file_path_);
    if (!in.is_open()) return false;

    std::string line;
    std::getline(in, line);
    relation_stats_.clear();

    if (line == "STATS_V1") {
        std::getline(in, line);
        size_t rel_count = static_cast<size_t>(std::stoull(line));
        for (size_t r = 0; r < rel_count; ++r) {
            std::getline(in, line);
            auto parts = SplitEscaped(line);
            if (parts.size() != 5 || parts[0] != "REL") throw std::runtime_error("Corrupt stats relation entry");
            RelationId rid = static_cast<RelationId>(std::stoul(parts[1]));
            RelationStats stats;
            stats.tuple_count = static_cast<size_t>(std::stoull(parts[2]));
            stats.page_count = static_cast<size_t>(std::stoull(parts[3]));
            size_t col_count = static_cast<size_t>(std::stoull(parts[4]));
            for (size_t c = 0; c < col_count; ++c) {
                std::getline(in, line);
                parts = SplitEscaped(line);
                if (parts.size() != 8 || parts[0] != "COL") throw std::runtime_error("Corrupt stats column entry");
                ColumnStats cs;
                size_t col_idx = static_cast<size_t>(std::stoull(parts[1]));
                cs.distinct_count = static_cast<size_t>(std::stoull(parts[2]));
                cs.null_fraction = std::stod(parts[3]);
                cs.has_numeric_minmax = std::stoi(parts[4]) != 0;
                TypeId t = static_cast<TypeId>(std::stoi(parts[5]));
                cs.min_value = ParseValue(t, parts[6]);
                cs.max_value = ParseValue(t, parts[7]);
                stats.columns[col_idx] = cs;
            }
            relation_stats_[rid] = std::move(stats);
        }
        return true;
    }

    if (line != "STATS_V2") throw std::runtime_error("Unsupported stats catalog format");

    std::getline(in, line);
    size_t rel_count = static_cast<size_t>(std::stoull(line));
    for (size_t r = 0; r < rel_count; ++r) {
        std::getline(in, line);
        auto parts = SplitEscaped(line);
        if (parts.size() != 6 || parts[0] != "REL") throw std::runtime_error("Corrupt stats relation entry");
        RelationId rid = static_cast<RelationId>(std::stoul(parts[1]));
        RelationStats stats;
        stats.tuple_count = static_cast<size_t>(std::stoull(parts[2]));
        stats.page_count = static_cast<size_t>(std::stoull(parts[3]));
        size_t col_count = static_cast<size_t>(std::stoull(parts[4]));
        size_t pair_count = static_cast<size_t>(std::stoull(parts[5]));

        for (size_t c = 0; c < col_count; ++c) {
            std::getline(in, line);
            parts = SplitEscaped(line);
            if (parts.size() != 10 || parts[0] != "COL") throw std::runtime_error("Corrupt stats column entry");
            ColumnStats cs;
            size_t col_idx = static_cast<size_t>(std::stoull(parts[1]));
            cs.distinct_count = static_cast<size_t>(std::stoull(parts[2]));
            cs.null_fraction = std::stod(parts[3]);
            cs.has_numeric_minmax = std::stoi(parts[4]) != 0;
            TypeId minmax_type = static_cast<TypeId>(std::stoi(parts[5]));
            cs.min_value = ParseValue(minmax_type, parts[6]);
            cs.max_value = ParseValue(minmax_type, parts[7]);
            size_t hist_count = static_cast<size_t>(std::stoull(parts[8]));
            size_t mcv_count = static_cast<size_t>(std::stoull(parts[9]));
            for (size_t h = 0; h < hist_count; h++) {
                std::getline(in, line);
                auto hist_parts = SplitEscaped(line);
                if (hist_parts.size() != 4 || hist_parts[0] != "HIST") throw std::runtime_error("Corrupt histogram entry");
                HistogramBucket bucket;
                TypeId bucket_type = static_cast<TypeId>(std::stoi(hist_parts[1]));
                bucket.upper_bound = ParseValue(bucket_type, hist_parts[2]);
                bucket.cumulative_fraction = std::stod(hist_parts[3]);
                cs.histogram.push_back(std::move(bucket));
            }
            for (size_t m = 0; m < mcv_count; m++) {
                std::getline(in, line);
                auto mcv_parts = SplitEscaped(line);
                if (mcv_parts.size() != 4 || mcv_parts[0] != "MCV") throw std::runtime_error("Corrupt MCV entry");
                MCVEntry entry;
                TypeId mcv_type = static_cast<TypeId>(std::stoi(mcv_parts[1]));
                entry.value = ParseValue(mcv_type, mcv_parts[2]);
                entry.frequency = std::stod(mcv_parts[3]);
                cs.mcv_entries.push_back(std::move(entry));
            }
            stats.columns[col_idx] = std::move(cs);
        }

        for (size_t p = 0; p < pair_count; p++) {
            std::getline(in, line);
            parts = SplitEscaped(line);
            if (parts.size() != 5 || parts[0] != "PAIR") throw std::runtime_error("Corrupt multicolumn stats entry");
            MultiColumnStats ms;
            ms.first_column_idx = static_cast<size_t>(std::stoull(parts[1]));
            ms.second_column_idx = static_cast<size_t>(std::stoull(parts[2]));
            ms.joint_distinct_count = static_cast<size_t>(std::stoull(parts[3]));
            size_t pair_mcv_count = static_cast<size_t>(std::stoull(parts[4]));
            for (size_t i = 0; i < pair_mcv_count; i++) {
                std::getline(in, line);
                auto pair_parts = SplitEscaped(line);
                if (pair_parts.size() != 6 || pair_parts[0] != "PAIRMCV") throw std::runtime_error("Corrupt multicolumn MCV entry");
                PairMCVEntry entry;
                entry.first_value = ParseValue(static_cast<TypeId>(std::stoi(pair_parts[1])), pair_parts[2]);
                entry.second_value = ParseValue(static_cast<TypeId>(std::stoi(pair_parts[3])), pair_parts[4]);
                entry.frequency = std::stod(pair_parts[5]);
                ms.mcv_pairs.push_back(std::move(entry));
            }
            stats.multi_column_stats[EncodeMultiColumnStatsKey(ms.first_column_idx, ms.second_column_idx)] = std::move(ms);
        }

        relation_stats_[rid] = std::move(stats);
    }
    return true;
}

}  // namespace simpledb
