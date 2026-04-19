#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "access/btree.h"
#include "catalog/catalog_manager.h"
#include "recovery/log_manager.h"
#include "storage/disk_manager.h"
#include "storage/relation_manager.h"
#include "storage/table.h"
#include "tools/benchmark_runner.h"
#include "tools/consistency_checker.h"

using namespace simpledb;

int main(int argc, char **argv) {
    std::string db_dir = (argc >= 2) ? argv[1] : "bench_db";
    int row_count = (argc >= 3) ? std::stoi(argv[2]) : 5000;
    int iters = (argc >= 4) ? std::stoi(argv[3]) : 5;

    std::filesystem::remove_all(db_dir);
    std::filesystem::create_directories(db_dir);

    try {
        DiskManager disk(db_dir);
        LogManager log(db_dir + "/wal.log");
        BufferPoolManager bpm(64, &disk, &log);
        RelationManager rel_mgr(&disk, &bpm);
        CatalogManager catalog(db_dir + "/catalog.txt");

        Schema schema({
            Column("id", TypeId::INT32, false),
            Column("dept_id", TypeId::INT32, false),
            Column("salary", TypeId::INT64, false)
        });

        RelationId heap_rel_id = rel_mgr.CreateHeapRelation("employees", schema, "employees.heap");
        HeapFile heap(&bpm, heap_rel_id, schema, rel_mgr.GetFreeSpaceMap(heap_rel_id), &log, nullptr);
        catalog.RegisterRelation(heap_rel_id, "employees", schema, &heap);

        RelationId index_rel_id = 2;
        disk.CreateRelation(index_rel_id, "employees_id.idx");
        BTreeIndex id_index(&bpm, index_rel_id, TypeId::INT32, 0, &log);
        catalog.RegisterIndex("employees_id_idx", heap_rel_id, index_rel_id, 0, TypeId::INT32, 0, &id_index, true);

        Table employees(&catalog, heap_rel_id);
        for (int i = 1; i <= row_count; i++) {
            Tuple tuple({Value(static_cast<int32_t>(i)),
                         Value(static_cast<int32_t>(i % 25)),
                         Value(static_cast<int64_t>(50000 + i))});
            employees.InsertTuple(tuple);
        }

        bpm.FlushAllPages();
        catalog.Save();

        std::vector<BenchmarkResult> results;
        results.push_back(BenchmarkRunner::BenchmarkSeqScan(heap, iters));
        results.push_back(BenchmarkRunner::BenchmarkPointLookup(
            heap, id_index, Value(static_cast<int32_t>(row_count / 2)), iters));
        results.push_back(BenchmarkRunner::BenchmarkRangeScan(
            id_index,
            Value(static_cast<int32_t>(row_count / 3)),
            Value(static_cast<int32_t>(row_count / 3 + 500)),
            iters));

        std::cout << BenchmarkRunner::FormatResults(results) << "\n";

        std::string check_error;
        bool ok = ConsistencyChecker::VerifyIndexAgainstHeap(heap, id_index, 0, &check_error);
        if (!ok) {
            std::cerr << "Consistency check failed: " << check_error << "\n";
            return 2;
        }

        std::cout << "Consistency check passed.\n";
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "db_bench failed: " << ex.what() << "\n";
        return 1;
    }
}
