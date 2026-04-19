#pragma once

#include <optional>
#include <string>
#include <vector>

#include "../access/btree.h"
#include "../access/heap_file.h"

namespace simpledb {

struct BenchmarkResult {
    std::string name;
    int iterations{0};
    double avg_ms{0.0};
    double min_ms{0.0};
    double max_ms{0.0};
    std::size_t rows_seen{0};
};

class BenchmarkRunner {
public:
    static BenchmarkResult BenchmarkSeqScan(const HeapFile &heap_file, int iterations);

    static BenchmarkResult BenchmarkPointLookup(const HeapFile &heap_file,
                                                const BTreeIndex &index,
                                                const Value &key,
                                                int iterations);

    static BenchmarkResult BenchmarkRangeScan(const BTreeIndex &index,
                                              std::optional<Value> lower_bound,
                                              std::optional<Value> upper_bound,
                                              int iterations);

    static std::string FormatResults(const std::vector<BenchmarkResult> &results);
};

}  // namespace simpledb
