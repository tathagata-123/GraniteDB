#include "benchmark_runner.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

#include "../access/btree_iterator.h"
#include "../access/heap_file_iterator.h"
#include "../execution/expressions.h"

namespace simpledb {

namespace {

template <typename Fn>
BenchmarkResult RunTimed(const std::string &name, int iterations, Fn fn) {
    BenchmarkResult result;
    result.name = name;
    result.iterations = iterations;
    result.min_ms = std::numeric_limits<double>::max();
    result.max_ms = 0.0;
    result.rows_seen = 0;

    double total_ms = 0.0;
    for (int i = 0; i < iterations; i++) {
        auto start = std::chrono::steady_clock::now();
        std::size_t rows = fn();
        auto end = std::chrono::steady_clock::now();

        double ms =
            std::chrono::duration<double, std::milli>(end - start).count();

        total_ms += ms;
        result.min_ms = std::min(result.min_ms, ms);
        result.max_ms = std::max(result.max_ms, ms);
        result.rows_seen = rows;
    }

    result.avg_ms = (iterations > 0) ? (total_ms / iterations) : 0.0;
    if (iterations == 0) {
        result.min_ms = 0.0;
    }
    return result;
}

}  // namespace

BenchmarkResult BenchmarkRunner::BenchmarkSeqScan(const HeapFile &heap_file, int iterations) {
    return RunTimed("SeqScan", iterations, [&]() -> std::size_t {
        std::size_t rows = 0;
        HeapFileIterator it(&heap_file);
        while (it.HasNext()) {
            auto [rid, tuple] = it.Next();
            (void)rid;
            (void)tuple;
            rows++;
        }
        return rows;
    });
}

BenchmarkResult BenchmarkRunner::BenchmarkPointLookup(const HeapFile &heap_file,
                                                      const BTreeIndex &index,
                                                      const Value &key,
                                                      int iterations) {
    return RunTimed("IndexPointLookup", iterations, [&]() -> std::size_t {
        std::size_t rows = 0;
        std::vector<RID> hits = index.Search(key);
        for (const RID &rid : hits) {
            Tuple tuple;
            if (heap_file.GetTuple(rid, &tuple)) {
                rows++;
            }
        }
        return rows;
    });
}

BenchmarkResult BenchmarkRunner::BenchmarkRangeScan(const BTreeIndex &index,
                                                    std::optional<Value> lower_bound,
                                                    std::optional<Value> upper_bound,
                                                    int iterations) {
    return RunTimed("IndexRangeScan", iterations, [&]() -> std::size_t {
        std::size_t rows = 0;

        BTreeIndexIterator it =
            lower_bound.has_value() ? BTreeIndexIterator(&index, *lower_bound)
                                    : BTreeIndexIterator(&index);

        while (it.HasNext()) {
            auto [key, rid] = it.Next();
            (void)rid;

            if (lower_bound.has_value() && CompareValues(key, *lower_bound) < 0) {
                continue;
            }
            if (upper_bound.has_value() && CompareValues(key, *upper_bound) > 0) {
                break;
            }

            rows++;
        }

        return rows;
    });
}

std::string BenchmarkRunner::FormatResults(const std::vector<BenchmarkResult> &results) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);

    for (const auto &r : results) {
        out << r.name
            << " | iterations=" << r.iterations
            << " | avg_ms=" << r.avg_ms
            << " | min_ms=" << r.min_ms
            << " | max_ms=" << r.max_ms
            << " | rows=" << r.rows_seen
            << "\n";
    }

    return out.str();
}

}  // namespace simpledb
