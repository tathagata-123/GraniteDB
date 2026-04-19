#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "access/btree.h"
#include "access/generic_btree.h"
#include "access/heap_file_iterator.h"
#include "access/index.h"
#include "concurrency/transaction_manager.h"
#include "recovery/checkpoint_manager.h"
#include "recovery/log_manager.h"
#include "recovery/recovery_manager.h"
#include "storage/disk_manager.h"
#include "storage/relation_manager.h"
#include "storage/table.h"
#include "tools/crash_harness.h"

using namespace simpledb;

namespace {

constexpr RelationId kHeapRelationId = 1;
constexpr RelationId kIdIndexRelationId = 2;
constexpr RelationId kDeptSalaryIndexRelationId = 3;

struct EmployeeRow {
    int dept_id;
    int64_t salary;
};

Schema BuildSchema() {
    return Schema({
        Column("id", TypeId::INT32, false),
        Column("dept_id", TypeId::INT32, false),
        Column("salary", TypeId::INT64, false)
    });
}

Tuple MakeEmployeeTuple(int id, int dept_id, int64_t salary) {
    return Tuple({Value(static_cast<int32_t>(id)),
                  Value(static_cast<int32_t>(dept_id)),
                  Value(static_cast<int64_t>(salary))});
}

EmployeeRow MakeSeedRow(int id) {
    return EmployeeRow{id % 19, 60000 + static_cast<int64_t>(id) * 100};
}

EmployeeRow MakeUpdatedRow(int id) {
    return EmployeeRow{(id + 5) % 23, 90000 + static_cast<int64_t>(id) * 111};
}

EmployeeRow MakeSplitRow(int id) {
    return EmployeeRow{(id * 7) % 29, 120000 + static_cast<int64_t>(id) * 73};
}

Tuple MakeSeedTuple(int id) {
    EmployeeRow row = MakeSeedRow(id);
    return MakeEmployeeTuple(id, row.dept_id, row.salary);
}

Tuple MakeUpdatedTuple(int id) {
    EmployeeRow row = MakeUpdatedRow(id);
    return MakeEmployeeTuple(id, row.dept_id, row.salary);
}

Tuple MakeSplitTuple(int id) {
    EmployeeRow row = MakeSplitRow(id);
    return MakeEmployeeTuple(id, row.dept_id, row.salary);
}

struct DemoEnv {
    DiskManager disk;
    LogManager log;
    BufferPoolManager bpm;
    LockManager lock_mgr;
    TransactionManager txn_mgr;
    RelationManager rel_mgr;
    CatalogManager catalog;
    Schema schema;
    std::unique_ptr<HeapFile> heap;
    std::unique_ptr<Table> table;
    std::unique_ptr<BTreeIndex> id_index_raw;
    std::unique_ptr<BTreeIndexAdapter> id_index;
    std::unique_ptr<GenericBTreeIndex> dept_salary_index;

    explicit DemoEnv(const std::string &db_dir)
        : disk(db_dir),
          log(db_dir + "/wal.log"),
          bpm(64, &disk, &log),
          lock_mgr(),
          txn_mgr(&lock_mgr, &log, &bpm),
          rel_mgr(&disk, &bpm),
          catalog(db_dir + "/catalog.meta"),
          schema(BuildSchema()) {}
};

[[noreturn]] void CrashNow() {
    std::fflush(nullptr);
    std::_Exit(88);
}

void RegisterRuntimeIndexes(DemoEnv &env) {
    IndexCatalogEntry id_idx;
    id_idx.index_name = "employees_id_idx";
    id_idx.base_relation_id = kHeapRelationId;
    id_idx.index_relation_id = kIdIndexRelationId;
    id_idx.index_file_name = "employees_id.idx";
    id_idx.is_unique = true;
    id_idx.null_policy = NullPolicy::NOT_SUPPORTED;
    id_idx.kind = IndexKind::BTREE;
    id_idx.key_columns.push_back(IndexKeyColumnDefinition{0, TypeId::INT32, 0, false});
    id_idx.root_page_no = env.id_index->GetRootPageNo();
    id_idx.runtime_index = env.id_index.get();
    env.catalog.RegisterIndex(id_idx);

    IndexCatalogEntry dept_salary_idx;
    dept_salary_idx.index_name = "employees_dept_salary_idx";
    dept_salary_idx.base_relation_id = kHeapRelationId;
    dept_salary_idx.index_relation_id = kDeptSalaryIndexRelationId;
    dept_salary_idx.index_file_name = "employees_dept_salary.idx";
    dept_salary_idx.is_unique = false;
    dept_salary_idx.null_policy = NullPolicy::NOT_SUPPORTED;
    dept_salary_idx.kind = IndexKind::BTREE;
    dept_salary_idx.key_columns.push_back(IndexKeyColumnDefinition{1, TypeId::INT32, 0, false});
    dept_salary_idx.key_columns.push_back(IndexKeyColumnDefinition{2, TypeId::INT64, 0, false});
    dept_salary_idx.root_page_no = env.dept_salary_index->GetRootPageNo();
    dept_salary_idx.runtime_index = env.dept_salary_index.get();
    env.catalog.RegisterIndex(dept_salary_idx);
}

void OpenFresh(DemoEnv &env) {
    RelationId heap_rel_id = env.rel_mgr.CreateHeapRelation("employees", env.schema, "employees.heap");
    if (heap_rel_id != kHeapRelationId) {
        throw std::runtime_error("Unexpected heap relation id");
    }
    env.heap = std::make_unique<HeapFile>(
        &env.bpm, heap_rel_id, env.schema, env.rel_mgr.GetFreeSpaceMap(heap_rel_id), &env.log, &env.lock_mgr);
    env.catalog.RegisterRelation(heap_rel_id, "employees", env.schema, env.heap.get(), "employees.heap");

    env.disk.CreateRelation(kIdIndexRelationId, "employees_id.idx");
    env.id_index_raw = std::make_unique<BTreeIndex>(&env.bpm, kIdIndexRelationId, TypeId::INT32, 0, &env.log);
    env.id_index = std::make_unique<BTreeIndexAdapter>(env.id_index_raw.get());

    env.disk.CreateRelation(kDeptSalaryIndexRelationId, "employees_dept_salary.idx");
    IndexDefinition def;
    def.index_name = "employees_dept_salary_idx";
    def.base_relation_id = kHeapRelationId;
    def.index_relation_id = kDeptSalaryIndexRelationId;
    def.is_unique = false;
    def.null_policy = NullPolicy::NOT_SUPPORTED;
    def.kind = IndexKind::BTREE;
    def.key_columns = {
        IndexKeyColumnDefinition{1, TypeId::INT32, 0, false},
        IndexKeyColumnDefinition{2, TypeId::INT64, 0, false}
    };
    env.dept_salary_index = std::make_unique<GenericBTreeIndex>(&env.bpm, def, &env.log);

    RegisterRuntimeIndexes(env);
    env.table = std::make_unique<Table>(&env.catalog, kHeapRelationId);
}

void OpenExisting(DemoEnv &env) {
    if (!env.catalog.Load()) {
        throw std::runtime_error("Failed to load durable catalog metadata for recovery bootstrap");
    }

    const RelationCatalogEntry &rel = env.catalog.GetRelation(kHeapRelationId);
    env.rel_mgr.RegisterExistingHeapRelation(rel.relation_id,
                                             rel.relation_name,
                                             rel.schema,
                                             rel.heap_file_name);
    env.rel_mgr.BuildFreeSpaceMap(rel.relation_id);
    env.heap = std::make_unique<HeapFile>(
        &env.bpm, rel.relation_id, rel.schema, env.rel_mgr.GetFreeSpaceMap(rel.relation_id), &env.log, &env.lock_mgr);
    env.catalog.AttachHeapFile(rel.relation_id, env.heap.get());

    const IndexCatalogEntry *id_meta = env.catalog.FindIndexByName(kHeapRelationId, "employees_id_idx");
    if (id_meta == nullptr) {
        throw std::runtime_error("Missing employees_id_idx metadata in durable catalog");
    }
    env.disk.OpenRelation(id_meta->index_relation_id, id_meta->index_file_name);
    env.id_index_raw = std::make_unique<BTreeIndex>(&env.bpm,
                                                    id_meta->index_relation_id,
                                                    id_meta->GetSingleKeyType(),
                                                    id_meta->GetSingleMaxVarcharLen(),
                                                    &env.log);
    env.id_index = std::make_unique<BTreeIndexAdapter>(env.id_index_raw.get());
    env.catalog.AttachIndex(kHeapRelationId, id_meta->index_name, env.id_index.get());

    const IndexCatalogEntry *dept_meta = env.catalog.FindIndexByName(kHeapRelationId, "employees_dept_salary_idx");
    if (dept_meta == nullptr) {
        throw std::runtime_error("Missing employees_dept_salary_idx metadata in durable catalog");
    }
    env.disk.OpenRelation(dept_meta->index_relation_id, dept_meta->index_file_name);
    IndexDefinition def;
    def.index_name = dept_meta->index_name;
    def.base_relation_id = dept_meta->base_relation_id;
    def.index_relation_id = dept_meta->index_relation_id;
    def.is_unique = dept_meta->is_unique;
    def.null_policy = dept_meta->null_policy;
    def.kind = dept_meta->kind;
    def.key_columns = dept_meta->key_columns;
    def.root_page_no = dept_meta->root_page_no;
    env.dept_salary_index = std::make_unique<GenericBTreeIndex>(&env.bpm, def, &env.log);
    env.catalog.AttachIndex(kHeapRelationId, dept_meta->index_name, env.dept_salary_index.get());

    env.table = std::make_unique<Table>(&env.catalog, kHeapRelationId);
}

void MaybeCheckpoint(DemoEnv &env, CheckpointManager &ckpt, int op_count) {
    if (op_count > 0 && op_count % 40 == 0) {
        ckpt.CreateCheckpoint();
    }
}

void RunMixedWorkload(DemoEnv &env, int base_row_count) {
    if (base_row_count < 30) {
        base_row_count = 30;
    }

    CheckpointManager ckpt(&env.log, &env.txn_mgr, &env.bpm);
    std::vector<RID> rid_by_id(static_cast<std::size_t>(base_row_count + base_row_count / 3 + 5));

    auto seed_txn = env.txn_mgr.Begin();
    int op_count = 0;
    for (int id = 1; id <= base_row_count; ++id) {
        RID rid = env.table->InsertTuple(seed_txn, MakeEmployeeTuple(id, id % 11, 50000 + id * 10));
        rid_by_id[static_cast<std::size_t>(id)] = rid;
        MaybeCheckpoint(env, ckpt, ++op_count);
    }
    if (!env.txn_mgr.Commit(seed_txn)) {
        throw std::runtime_error("Seed transaction commit failed");
    }

    ckpt.CreateCheckpoint();

    auto txn = env.txn_mgr.Begin();
    op_count = 0;

    for (int id = 3; id <= base_row_count; id += 3) {
        RID current = rid_by_id[static_cast<std::size_t>(id)];
        RID new_rid{};
        bool ok = env.table->UpdateTuple(txn,
                                         current,
                                         MakeEmployeeTuple(id, (id + 3) % 13, 70000 + id * 25),
                                         &new_rid);
        if (!ok) {
            throw std::runtime_error("Update failed in mixed workload");
        }
        rid_by_id[static_cast<std::size_t>(id)] = new_rid;
        MaybeCheckpoint(env, ckpt, ++op_count);
    }

    for (int id = 5; id <= base_row_count; id += 5) {
        RID current = rid_by_id[static_cast<std::size_t>(id)];
        if (current.page_no == 0 && current.slot_no == 0) {
            continue;
        }
        if (!env.table->DeleteTuple(txn, current)) {
            throw std::runtime_error("Delete failed in mixed workload");
        }
        rid_by_id[static_cast<std::size_t>(id)] = RID{};
        MaybeCheckpoint(env, ckpt, ++op_count);
    }

    const int extra_begin = base_row_count + 1;
    const int extra_end = base_row_count + base_row_count / 4;
    if (static_cast<int>(rid_by_id.size()) <= extra_end) {
        rid_by_id.resize(static_cast<std::size_t>(extra_end + 1));
    }
    for (int id = extra_begin; id <= extra_end; ++id) {
        RID rid = env.table->InsertTuple(txn, MakeEmployeeTuple(id, (id * 7) % 17, 90000 + id));
        rid_by_id[static_cast<std::size_t>(id)] = rid;
        MaybeCheckpoint(env, ckpt, ++op_count);
    }

    if (!env.txn_mgr.Commit(txn)) {
        throw std::runtime_error("Mixed transaction commit failed");
    }
}

std::unordered_map<int, EmployeeRow> BuildExpectedSingleRow() {
    return {{1, EmployeeRow{7, 100001}}};
}

std::unordered_map<int, EmployeeRow> BuildExpectedSplitRows(int row_count) {
    std::unordered_map<int, EmployeeRow> expected;
    expected.reserve(static_cast<std::size_t>(row_count));
    for (int id = 1; id <= row_count; ++id) {
        expected.emplace(id, MakeSplitRow(id));
    }
    return expected;
}

std::unordered_map<int, EmployeeRow> BuildExpectedLoserRows(int seed_count) {
    std::unordered_map<int, EmployeeRow> expected;
    expected.reserve(static_cast<std::size_t>(seed_count));
    for (int id = 1; id <= seed_count; ++id) {
        expected.emplace(id, MakeSeedRow(id));
    }
    return expected;
}

void VerifyRecoveredLogicalState(DemoEnv &env,
                                 const std::unordered_map<int, EmployeeRow> &expected) {
    std::unordered_map<int, RID> actual_rids;
    actual_rids.reserve(expected.size());

    HeapFileIterator it(env.heap.get());
    while (it.HasNext()) {
        auto [rid, tuple] = it.Next();
        if (tuple.Size() != 3) {
            throw std::runtime_error("Recovered tuple has unexpected arity");
        }

        int id = tuple.GetValue(0).AsInt32();
        int dept_id = tuple.GetValue(1).AsInt32();
        int64_t salary = tuple.GetValue(2).AsInt64();

        auto exp_it = expected.find(id);
        if (exp_it == expected.end()) {
            throw std::runtime_error("Recovered unexpected employee id " + std::to_string(id));
        }
        if (actual_rids.find(id) != actual_rids.end()) {
            throw std::runtime_error("Recovered duplicate employee id " + std::to_string(id));
        }
        if (dept_id != exp_it->second.dept_id || salary != exp_it->second.salary) {
            throw std::runtime_error("Recovered wrong values for employee id " + std::to_string(id));
        }
        actual_rids.emplace(id, rid);
    }

    if (actual_rids.size() != expected.size()) {
        throw std::runtime_error("Recovered row-count mismatch: expected " +
                                 std::to_string(expected.size()) + ", got " +
                                 std::to_string(actual_rids.size()));
    }

    for (const auto &[id, row] : expected) {
        auto rid_it = actual_rids.find(id);
        if (rid_it == actual_rids.end()) {
            throw std::runtime_error("Recovered state is missing employee id " + std::to_string(id));
        }

        const RID &rid = rid_it->second;
        Tuple tuple;
        if (!env.heap->GetTuple(rid, &tuple)) {
            throw std::runtime_error("Recovered RID lookup failed for employee id " + std::to_string(id));
        }
        if (tuple.GetValue(0).AsInt32() != id ||
            tuple.GetValue(1).AsInt32() != row.dept_id ||
            tuple.GetValue(2).AsInt64() != row.salary) {
            throw std::runtime_error("Recovered heap RID points to the wrong tuple for employee id " +
                                     std::to_string(id));
        }

        std::vector<RID> id_hits = env.id_index->SearchExact({Value(static_cast<int32_t>(id))});
        if (id_hits.size() != 1 || !(id_hits[0] == rid)) {
            throw std::runtime_error("ID index mismatch for employee id " + std::to_string(id));
        }

        std::vector<RID> composite_hits = env.dept_salary_index->SearchExact(
            {Value(static_cast<int32_t>(row.dept_id)), Value(static_cast<int64_t>(row.salary))});
        if (composite_hits.size() != 1 || !(composite_hits[0] == rid)) {
            throw std::runtime_error("Composite index mismatch for employee id " + std::to_string(id));
        }
    }
}

void RecoverVerifyAndCheckExact(DemoEnv &env,
                                const std::unordered_map<int, EmployeeRow> &expected) {
    RecoveryManager recovery(&env.bpm, &env.log);
    std::vector<IndexCheckSpec> indexes = {
        {"employees_id_idx", env.id_index.get(), {0}},
        {"employees_dept_salary_idx", env.dept_salary_index.get(), {1, 2}}
    };

    std::string report;
    bool ok = CrashHarness::RecoverAndVerify(recovery, *env.heap, indexes, &report);
    std::cout << report;
    if (!ok) {
        throw std::runtime_error("CrashHarness recovery verification failed");
    }

    VerifyRecoveredLogicalState(env, expected);
    std::cout << "Exact logical-content verification passed.\n";
}

void PrepareFreshDb(DemoEnv *env) {
    CrashHarness::DisableCrashPoint();
    OpenFresh(*env);
    env->bpm.FlushAllPages();
}

void RunExactNewPageSetup(const std::string &db_dir) {
    std::filesystem::remove_all(db_dir);
    std::filesystem::create_directories(db_dir);
    DemoEnv env(db_dir);
    PrepareFreshDb(&env);

    auto txn = env.txn_mgr.Begin();
    RID rid = env.table->InsertTuple(txn, MakeEmployeeTuple(1, 7, 100001));
    (void)rid;
    if (!env.txn_mgr.Commit(txn)) {
        throw std::runtime_error("Commit failed in exact_new_page_setup");
    }

    CheckpointManager ckpt(&env.log, &env.txn_mgr, &env.bpm);
    ckpt.CreateCheckpoint();
    CrashNow();
}

void RunExactSplitSetup(const std::string &db_dir, int row_count) {
    if (row_count < 200) {
        row_count = 200;
    }

    std::filesystem::remove_all(db_dir);
    std::filesystem::create_directories(db_dir);
    DemoEnv env(db_dir);
    PrepareFreshDb(&env);

    auto txn = env.txn_mgr.Begin();
    for (int id = 1; id <= row_count; ++id) {
        env.table->InsertTuple(txn, MakeSplitTuple(id));
    }
    if (!env.txn_mgr.Commit(txn)) {
        throw std::runtime_error("Commit failed in exact_split_setup");
    }

    CheckpointManager ckpt(&env.log, &env.txn_mgr, &env.bpm);
    ckpt.CreateCheckpoint();
    CrashNow();
}

void RunExactLoserSetup(const std::string &db_dir, int seed_count) {
    if (seed_count < 80) {
        seed_count = 80;
    }

    std::filesystem::remove_all(db_dir);
    std::filesystem::create_directories(db_dir);
    DemoEnv env(db_dir);
    PrepareFreshDb(&env);

    std::vector<RID> rid_by_id(static_cast<std::size_t>(seed_count + 64));

    auto seed_txn = env.txn_mgr.Begin();
    for (int id = 1; id <= seed_count; ++id) {
        rid_by_id[static_cast<std::size_t>(id)] = env.table->InsertTuple(seed_txn, MakeSeedTuple(id));
    }
    if (!env.txn_mgr.Commit(seed_txn)) {
        throw std::runtime_error("Seed commit failed in exact_loser_setup");
    }

    CheckpointManager ckpt(&env.log, &env.txn_mgr, &env.bpm);
    ckpt.CreateCheckpoint();

    auto loser = env.txn_mgr.Begin();

    for (int id = 4; id <= seed_count; id += 4) {
        RID new_rid{};
        if (!env.table->UpdateTuple(loser,
                                    rid_by_id[static_cast<std::size_t>(id)],
                                    MakeUpdatedTuple(id),
                                    &new_rid)) {
            throw std::runtime_error("Loser update failed for employee id " + std::to_string(id));
        }
        rid_by_id[static_cast<std::size_t>(id)] = new_rid;
    }

    for (int id = 9; id <= seed_count; id += 9) {
        if (!env.table->DeleteTuple(loser, rid_by_id[static_cast<std::size_t>(id)])) {
            throw std::runtime_error("Loser delete failed for employee id " + std::to_string(id));
        }
        rid_by_id[static_cast<std::size_t>(id)] = RID{};
    }

    for (int id = seed_count + 1; id <= seed_count + 24; ++id) {
        env.table->InsertTuple(loser, MakeUpdatedTuple(id));
    }

    env.log.FlushUpTo(loser->GetLastLSN());
    CrashNow();
}


namespace {

constexpr RelationId kStructuralLoserIndexRelationId = 41;
constexpr const char *kStructuralLoserIndexFile = "structural_loser.idx";

RID MakeStructuralLoserRid(int id) {
    return RID{static_cast<PageNo>(id), static_cast<SlotNo>(id % 1024)};
}

struct StructuralLoserIndexEnv {
    DiskManager disk;
    LogManager log;
    BufferPoolManager bpm;
    LockManager lock_mgr;
    TransactionManager txn_mgr;
    std::unique_ptr<BTreeIndex> index;

    explicit StructuralLoserIndexEnv(const std::string &db_dir)
        : disk(db_dir),
          log(db_dir + "/wal.log"),
          bpm(64, &disk, &log),
          lock_mgr(),
          txn_mgr(&lock_mgr, &log, &bpm) {}
};

void OpenFreshStructuralLoserEnv(StructuralLoserIndexEnv *env) {
    CrashHarness::DisableCrashPoint();
    env->disk.CreateRelation(kStructuralLoserIndexRelationId, kStructuralLoserIndexFile);
    env->index = std::make_unique<BTreeIndex>(&env->bpm,
                                              kStructuralLoserIndexRelationId,
                                              TypeId::INT32,
                                              0,
                                              &env->log);
    env->bpm.FlushAllPages();
}

void OpenExistingStructuralLoserEnv(StructuralLoserIndexEnv *env) {
    env->disk.OpenRelation(kStructuralLoserIndexRelationId, kStructuralLoserIndexFile);
    env->index = std::make_unique<BTreeIndex>(&env->bpm,
                                              kStructuralLoserIndexRelationId,
                                              TypeId::INT32,
                                              0,
                                              &env->log);
}

}  // namespace

void RunExactStructuralLoserSetup(const std::string &db_dir, int seed_count) {
    if (seed_count < 600) {
        seed_count = 600;
    }

    constexpr int kKeepRows = 10;

    std::filesystem::remove_all(db_dir);
    std::filesystem::create_directories(db_dir);
    StructuralLoserIndexEnv env(db_dir);
    OpenFreshStructuralLoserEnv(&env);

    auto seed_txn = env.txn_mgr.Begin();
    for (int id = 1; id <= seed_count; ++id) {
        env.index->Insert(seed_txn, Value(static_cast<int32_t>(id)), MakeStructuralLoserRid(id));
    }
    if (!env.txn_mgr.Commit(seed_txn)) {
        throw std::runtime_error("Seed commit failed in exact_structural_loser_setup");
    }

    CheckpointManager ckpt(&env.log, &env.txn_mgr, &env.bpm);
    ckpt.CreateCheckpoint();

    auto loser = env.txn_mgr.Begin();
    for (int id = seed_count; id > kKeepRows; --id) {
        if (!env.index->Delete(loser, Value(static_cast<int32_t>(id)), MakeStructuralLoserRid(id))) {
            throw std::runtime_error("Structural loser delete failed for key " + std::to_string(id));
        }
    }

    env.log.FlushUpTo(loser->GetLastLSN());
    CrashNow();
}

void RecoverVerifyStructuralLoser(const std::string &db_dir, int seed_count) {
    StructuralLoserIndexEnv env(db_dir);
    OpenExistingStructuralLoserEnv(&env);

    RecoveryManager recovery(&env.bpm, &env.log);
    recovery.Recover();

    for (int id = 1; id <= seed_count; ++id) {
        RID expected = MakeStructuralLoserRid(id);
        std::vector<RID> hits = env.index->Search(Value(static_cast<int32_t>(id)));
        if (hits.size() != 1 || !(hits[0] == expected)) {
            throw std::runtime_error("Structural loser recovery mismatch for key " + std::to_string(id));
        }
    }
}

void PrintUsage() {
    std::cout << "Usage:\n"
              << "  db_crash_demo fresh <db_dir> [row_count]\n"
              << "  db_crash_demo recover <db_dir>\n"
              << "  db_crash_demo exact_new_page_setup <db_dir>\n"
              << "  db_crash_demo exact_new_page_recover <db_dir>\n"
              << "  db_crash_demo exact_split_setup <db_dir> [row_count]\n"
              << "  db_crash_demo exact_split_recover <db_dir> [row_count]\n"
              << "  db_crash_demo exact_loser_setup <db_dir> [seed_count]\n"
              << "  db_crash_demo exact_loser_recover <db_dir> [seed_count]\n\n"
              << "Crash points include BEFORE_WAL_APPEND, AFTER_WAL_APPEND, BEFORE_WAL_FLUSH, AFTER_WAL_FLUSH,\n"
              << "BEFORE_PAGE_FLUSH, AFTER_PAGE_FLUSH, BEFORE_REDO_APPLY, AFTER_REDO_APPLY,\n"
              << "BEFORE_UNDO_APPLY, AFTER_UNDO_APPLY.\n";
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        PrintUsage();
        return 1;
    }

    std::string mode = argv[1];
    std::string db_dir = argv[2];

    try {
        if (mode == "fresh") {
            int row_count = (argc >= 4) ? std::stoi(argv[3]) : 200;
            std::filesystem::remove_all(db_dir);
            std::filesystem::create_directories(db_dir);

            const char *wanted_point = std::getenv("SIMPLEDB_CRASH_POINT");
            const char *wanted_after = std::getenv("SIMPLEDB_CRASH_AFTER");
            std::string saved_point = wanted_point != nullptr ? wanted_point : "";
            std::string saved_after = wanted_after != nullptr ? wanted_after : "";

            CrashHarness::DisableCrashPoint();

            DemoEnv env(db_dir);
            OpenFresh(env);
            env.bpm.FlushAllPages();

            if (!saved_point.empty()) {
                setenv("SIMPLEDB_CRASH_POINT", saved_point.c_str(), 1);
                if (!saved_after.empty()) {
                    setenv("SIMPLEDB_CRASH_AFTER", saved_after.c_str(), 1);
                }
            }

            CrashHarness::RunMutatingWorkload([&]() {
                RunMixedWorkload(env, row_count);
            });

            env.bpm.FlushAllPages();
            std::cout << "Fresh workload completed without crash.\n";
            return 0;
        }

        if (mode == "recover") {
            DemoEnv env(db_dir);
            OpenExisting(env);

            RecoveryManager recovery(&env.bpm, &env.log);
            std::vector<IndexCheckSpec> indexes = {
                {"employees_id_idx", env.id_index.get(), {0}},
                {"employees_dept_salary_idx", env.dept_salary_index.get(), {1, 2}}
            };

            std::string report;
            bool ok = CrashHarness::RecoverAndVerify(recovery, *env.heap, indexes, &report);
            std::cout << report;
            return ok ? 0 : 2;
        }

        if (mode == "exact_new_page_setup") {
            RunExactNewPageSetup(db_dir);
        }

        if (mode == "exact_new_page_recover") {
            DemoEnv env(db_dir);
            OpenExisting(env);
            RecoverVerifyAndCheckExact(env, BuildExpectedSingleRow());
            return 0;
        }

        if (mode == "exact_split_setup") {
            int row_count = (argc >= 4) ? std::stoi(argv[3]) : 260;
            RunExactSplitSetup(db_dir, row_count);
        }

        if (mode == "exact_split_recover") {
            int row_count = (argc >= 4) ? std::stoi(argv[3]) : 260;
            DemoEnv env(db_dir);
            OpenExisting(env);
            RecoverVerifyAndCheckExact(env, BuildExpectedSplitRows(row_count));
            return 0;
        }

        if (mode == "exact_loser_setup") {
            int seed_count = (argc >= 4) ? std::stoi(argv[3]) : 120;
            RunExactLoserSetup(db_dir, seed_count);
        }

        if (mode == "exact_loser_recover") {
            int seed_count = (argc >= 4) ? std::stoi(argv[3]) : 120;
            DemoEnv env(db_dir);
            OpenExisting(env);
            RecoverVerifyAndCheckExact(env, BuildExpectedLoserRows(seed_count));
            return 0;
        }

        if (mode == "exact_structural_loser_setup") {
            int seed_count = (argc >= 4) ? std::stoi(argv[3]) : 1200;
            RunExactStructuralLoserSetup(db_dir, seed_count);
        }

        if (mode == "exact_structural_loser_recover") {
            int seed_count = (argc >= 4) ? std::stoi(argv[3]) : 1200;
            RecoverVerifyStructuralLoser(db_dir, seed_count);
            return 0;
        }

        PrintUsage();
        return 1;
    } catch (const std::exception &ex) {
        std::cerr << "db_crash_demo failed: " << ex.what() << "\n";
        return 1;
    }
}
