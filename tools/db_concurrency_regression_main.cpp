#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <random>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../access/btree_iterator.h"
#include "../access/heap_file_iterator.h"
#include "../buffer/buffer_pool_manager.h"
#include "../catalog/catalog_manager.h"
#include "../concurrency/transaction_manager.h"
#include "../execution/operators.h"
#include "../recovery/log_manager.h"
#include "../storage/relation_manager.h"
#include "../storage/table.h"
#include "../tools/consistency_checker.h"

using namespace simpledb;

namespace {

void Expect(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

struct TestEnv {
    std::string db_dir;
    DiskManager disk_manager;
    LogManager log_manager;
    BufferPoolManager buffer_pool_manager;
    RelationManager relation_manager;
    CatalogManager catalog;
    LockManager lock_manager;
    TransactionManager txn_manager;
    Schema schema;
    RelationId relation_id{0};
    RelationId index_relation_id{0};
    std::unique_ptr<HeapFile> heap;
    std::unique_ptr<BTreeIndex> index;
    std::unique_ptr<Table> table;

    TestEnv(const std::string &dir, bool unique_index)
        : db_dir(dir),
          disk_manager(dir),
          log_manager(dir + "/wal.log"),
          buffer_pool_manager(128, &disk_manager, &log_manager),
          relation_manager(&disk_manager, &buffer_pool_manager),
          catalog(dir + "/catalog.meta"),
          lock_manager(50),
          txn_manager(&lock_manager, &log_manager, &buffer_pool_manager),
          schema({Column("id", TypeId::INT32, false), Column("payload", TypeId::INT32, false)}) {
        relation_id = relation_manager.CreateHeapRelation("t", schema);
        heap = std::make_unique<HeapFile>(&buffer_pool_manager,
                                          relation_id,
                                          schema,
                                          relation_manager.GetFreeSpaceMap(relation_id),
                                          &log_manager,
                                          &lock_manager);
        catalog.RegisterRelation(relation_id, "t", schema, heap.get(), "t.heap");

        index_relation_id = relation_manager.AllocateRelationId();
        disk_manager.CreateRelation(index_relation_id, "t_id.idx");
        index = std::make_unique<BTreeIndex>(&buffer_pool_manager, index_relation_id, TypeId::INT32, 0, &log_manager);
        catalog.RegisterIndex("t_id_idx", relation_id, index_relation_id, 0, TypeId::INT32, 0, index.get(), unique_index, NullPolicy::NOT_SUPPORTED, "t_id.idx");

        table = std::make_unique<Table>(&catalog, relation_id);
    }

    ~TestEnv() {
        buffer_pool_manager.FlushAllPages();
        disk_manager.Shutdown();
    }

    RID InsertRow(const TransactionPtr &txn, int id, int payload) {
        return table->InsertTuple(txn, Tuple({Value(static_cast<int32_t>(id)), Value(static_cast<int32_t>(payload))}));
    }

    int CountHeapRows() const {
        int count = 0;
        HeapFileIterator it(heap.get());
        while (it.HasNext()) {
            (void)it.Next();
            ++count;
        }
        return count;
    }
};

void TestSingleUpgraderRule() {
    LockManager lock_manager(50);
    LogManager log_manager("/tmp/simpledb_upgrade_rule_wal.log");
    TransactionManager txn_manager(&lock_manager, &log_manager, nullptr);
    RelationId rel = 1;
    RecordLockId rid{rel, RID{1, 1}};

    auto t1 = txn_manager.Begin();
    auto t2 = txn_manager.Begin();
    Expect(lock_manager.LockTable(t1, rel, LockMode::IX), "t1 IX lock failed");
    Expect(lock_manager.LockTable(t2, rel, LockMode::IX), "t2 IX lock failed");
    Expect(lock_manager.LockRecord(t1, rid, LockMode::S), "t1 S lock failed");
    Expect(lock_manager.LockRecord(t2, rid, LockMode::S), "t2 S lock failed");

    auto f1 = std::async(std::launch::async, [&] { return lock_manager.LockRecord(t1, rid, LockMode::X); });
    auto f2 = std::async(std::launch::async, [&] { return lock_manager.LockRecord(t2, rid, LockMode::X); });

    bool r2 = f2.get();
    if (!r2) {
        txn_manager.Abort(t2);
    }
    bool r1 = f1.get();

    Expect(r1 != r2, "Exactly one upgrader should succeed");
    if (r1) txn_manager.Commit(t1); else txn_manager.Abort(t1);
    if (r2) txn_manager.Commit(t2);
}

void TestDeadlockDetection() {
    LockManager lock_manager(50);
    LogManager log_manager("/tmp/simpledb_deadlock_wal.log");
    TransactionManager txn_manager(&lock_manager, &log_manager, nullptr);
    RelationId rel = 7;
    RecordLockId a{rel, RID{1, 1}};
    RecordLockId b{rel, RID{1, 2}};

    auto t1 = txn_manager.Begin();
    auto t2 = txn_manager.Begin();
    Expect(lock_manager.LockTable(t1, rel, LockMode::IX), "t1 table IX failed");
    Expect(lock_manager.LockTable(t2, rel, LockMode::IX), "t2 table IX failed");
    Expect(lock_manager.LockRecord(t1, a, LockMode::X), "t1 record a failed");
    Expect(lock_manager.LockRecord(t2, b, LockMode::X), "t2 record b failed");

    auto f1 = std::async(std::launch::async, [&] { return lock_manager.LockRecord(t1, b, LockMode::X); });
    auto f2 = std::async(std::launch::async, [&] { return lock_manager.LockRecord(t2, a, LockMode::X); });

    bool r1 = f1.get();
    bool r2 = f2.get();
    Expect(r1 != r2, "Deadlock detector should pick exactly one victim");

    if (!txn_manager.Commit(t1)) txn_manager.Abort(t1);
    if (!txn_manager.Commit(t2)) txn_manager.Abort(t2);
}

void TestDeadlockVictimHoldsLocksUntilRollbackCompletes() {
    LockManager lock_manager(25);
    LogManager log_manager("/tmp/simpledb_deadlock_strict2pl_wal.log");
    TransactionManager txn_manager(&lock_manager, &log_manager, nullptr);

    std::atomic<bool> callback_entered{false};
    std::atomic<bool> callback_finished{false};
    lock_manager.SetAbortCallback([&](const TransactionPtr &txn) {
        callback_entered.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        txn_manager.Abort(txn);
        callback_finished.store(true);
    });

    RelationId rel = 17;
    RecordLockId a{rel, RID{1, 1}};
    RecordLockId b{rel, RID{1, 2}};

    auto t1 = txn_manager.Begin();
    auto t2 = txn_manager.Begin();
    Expect(lock_manager.LockTable(t1, rel, LockMode::IX), "t1 table IX failed");
    Expect(lock_manager.LockTable(t2, rel, LockMode::IX), "t2 table IX failed");
    Expect(lock_manager.LockRecord(t1, a, LockMode::X), "t1 record a failed");
    Expect(lock_manager.LockRecord(t2, b, LockMode::X), "t2 record b failed");

    auto survivor_wait = std::async(std::launch::async, [&] { return lock_manager.LockRecord(t1, b, LockMode::X); });
    auto victim_wait = std::async(std::launch::async, [&] { return lock_manager.LockRecord(t2, a, LockMode::X); });

    for (int i = 0; i < 40 && !callback_entered.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    Expect(callback_entered.load(), "deadlock abort callback did not fire");
    Expect(!callback_finished.load(), "deadlock abort callback finished unexpectedly early");
    Expect(survivor_wait.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready,
           "survivor lock request should stay blocked until victim rollback completes");

    bool victim_result = victim_wait.get();
    Expect(!victim_result, "deadlock victim should fail its waited lock request");

    bool survivor_result = survivor_wait.get();
    Expect(survivor_result, "survivor should acquire the victim-held lock after rollback completes");
    Expect(callback_finished.load(), "deadlock abort callback should finish before survivor proceeds");

    Expect(txn_manager.Commit(t1), "survivor commit failed in strict-2PL deadlock test");
    if (!txn_manager.Commit(t2)) {
        txn_manager.Abort(t2);
    }
}

void TestThreeWayDeadlockDetection() {
    LockManager lock_manager(50);
    LogManager log_manager("/tmp/simpledb_deadlock3_wal.log");
    TransactionManager txn_manager(&lock_manager, &log_manager, nullptr);
    RelationId rel = 8;
    RecordLockId a{rel, RID{1, 1}};
    RecordLockId b{rel, RID{1, 2}};
    RecordLockId c{rel, RID{1, 3}};

    auto t1 = txn_manager.Begin();
    auto t2 = txn_manager.Begin();
    auto t3 = txn_manager.Begin();
    Expect(lock_manager.LockTable(t1, rel, LockMode::IX), "t1 table IX failed");
    Expect(lock_manager.LockTable(t2, rel, LockMode::IX), "t2 table IX failed");
    Expect(lock_manager.LockTable(t3, rel, LockMode::IX), "t3 table IX failed");
    Expect(lock_manager.LockRecord(t1, a, LockMode::X), "t1 record a failed");
    Expect(lock_manager.LockRecord(t2, b, LockMode::X), "t2 record b failed");
    Expect(lock_manager.LockRecord(t3, c, LockMode::X), "t3 record c failed");

    auto f1 = std::async(std::launch::async, [&] { return lock_manager.LockRecord(t1, b, LockMode::X); });
    auto f2 = std::async(std::launch::async, [&] { return lock_manager.LockRecord(t2, c, LockMode::X); });
    auto f3 = std::async(std::launch::async, [&] { return lock_manager.LockRecord(t3, a, LockMode::X); });

    for (int i = 0; i < 40 && !t3->IsAbortPendingOrDone(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    Expect(t3->IsAbortPendingOrDone(),
           "Three-way deadlock should pick the highest transaction id as victim");

    bool r3 = f3.get();
    Expect(!r3, "Three-way deadlock victim should fail its waited lock request");

    bool r2 = f2.get();
    Expect(r2, "middle transaction should proceed after victim aborts");
    Expect(txn_manager.Commit(t2), "middle transaction commit failed after three-way deadlock resolution");

    bool r1 = f1.get();
    Expect(r1, "remaining transaction should proceed after predecessor commits");
    Expect(txn_manager.Commit(t1), "leading transaction commit failed after three-way deadlock resolution");

    if (!txn_manager.Commit(t3)) txn_manager.Abort(t3);
}

void TestPhantomPreventionOnIndexedRange() {
    std::string dir = "/tmp/simpledb_phantom_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    TestEnv env(dir, false);

    {
        auto txn = env.txn_manager.Begin();
        env.InsertRow(txn, 10, 100);
        env.InsertRow(txn, 20, 200);
        Expect(env.txn_manager.Commit(txn), "seed commit failed");
    }

    auto reader = env.txn_manager.Begin();
    IndexScanExecutor scan(env.heap.get(), env.index.get(), Value(static_cast<int32_t>(10)), Value(static_cast<int32_t>(20)), true, true, reader, &env.lock_manager);
    scan.Init();
    Tuple tmp;
    int seen = 0;
    while (scan.Next(&tmp)) ++seen;
    Expect(seen == 2, "reader should see two seeded rows");

    auto writer = std::async(std::launch::async, [&] {
        auto txn = env.txn_manager.Begin();
        bool got_key_lock = env.lock_manager.LockKey(
            txn, env.index_relation_id, {Value(static_cast<int32_t>(15))}, LockMode::X);
        if (!got_key_lock) {
            env.txn_manager.Abort(txn);
            return false;
        }
        env.InsertRow(txn, 15, 150);
        bool ok = env.txn_manager.Commit(txn);
        return ok;
    });

    auto status = writer.wait_for(std::chrono::milliseconds(250));
    Expect(status != std::future_status::ready,
           "writer should still be blocked by the reader's key-range lock");

    Expect(env.txn_manager.Commit(reader), "reader commit failed");
    bool writer_ok = writer.get();
    Expect(writer_ok, "writer commit failed after reader released range lock");
    Expect(env.index->Search(Value(static_cast<int32_t>(15))).size() == 1,
           "inserted phantom row should become visible after reader commits");
}

void TestConcurrentUniqueInsertRace() {
    std::string dir = "/tmp/simpledb_unique_race_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    TestEnv env(dir, true);

    std::promise<void> start;
    std::shared_future<void> go(start.get_future());

    auto worker = [&](int payload) {
        auto txn = env.txn_manager.Begin();
        try {
            go.wait();
            env.InsertRow(txn, 42, payload);
            bool ok = env.txn_manager.Commit(txn);
            return ok;
        } catch (...) {
            env.txn_manager.Abort(txn);
            return false;
        }
    };

    auto f1 = std::async(std::launch::async, worker, 100);
    auto f2 = std::async(std::launch::async, worker, 200);
    start.set_value();

    bool r1 = f1.get();
    bool r2 = f2.get();
    std::size_t hits = env.index->Search(Value(static_cast<int32_t>(42))).size();
    int heap_rows = env.CountHeapRows();
    Expect(r1 != r2, "Exactly one concurrent unique insert should commit");
    Expect(hits == 1, "Unique index should contain exactly one winner row");
    Expect(heap_rows == 1, "Heap should contain exactly one winner row");
}

void TestAbortAfterSplitUndo() {
    std::string dir = "/tmp/simpledb_abort_split_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    TestEnv env(dir, false);

    auto txn = env.txn_manager.Begin();
    for (int i = 1; i <= 300; ++i) {
        env.InsertRow(txn, i, i * 10);
    }
    Expect(env.txn_manager.Abort(txn), "abort after split-heavy insert workload failed");
    Expect(env.CountHeapRows() == 0, "Abort should roll back all heap inserts");
    Expect(env.index->Search(Value(static_cast<int32_t>(42))).empty(),
           "Abort should roll back split-heavy index inserts");
}


void TestDeleteMergeAndScanSmoke() {
    std::string dir = "/tmp/simpledb_delete_scan_smoke";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    TestEnv env(dir, false);

    {
        auto txn = env.txn_manager.Begin();
        for (int i = 1; i <= 400; ++i) {
            env.InsertRow(txn, i, i * 10);
        }
        Expect(env.txn_manager.Commit(txn), "seed commit for delete smoke failed");
    }

    auto reader = std::async(std::launch::async, [&] {
        BTreeIndexIterator it(env.index.get());
        int count = 0;
        int last = 0;
        while (it.HasNext()) {
            try {
                auto [key, rid] = it.Next();
                int current = key.AsInt32();
                Expect(current >= last, "Iterator order should remain nondecreasing during concurrent delete/merge");
                last = current;
                (void)rid;
                ++count;
            } catch (const std::runtime_error &ex) {
                std::string msg = ex.what();
                if (msg.find("at end") != std::string::npos) break;
                throw;
            }
        }
        return count;
    });

    auto writer = std::async(std::launch::async, [&] {
        auto txn = env.txn_manager.Begin();
        for (int i = 50; i <= 350; ++i) {
            auto hits = env.index->Search(Value(static_cast<int32_t>(i)));
            for (const auto &rid : hits) {
                env.table->DeleteTuple(txn, rid);
            }
        }
        return env.txn_manager.Commit(txn);
    });

    int reader_count = reader.get();
    bool writer_ok = writer.get();
    Expect(reader_count >= 1, "Reader should traverse at least one entry during delete/merge smoke test");
    Expect(writer_ok, "Writer commit failed during delete/merge smoke test");
    Expect(env.index->Search(Value(static_cast<int32_t>(10))).size() == 1,
           "Low key should remain searchable after delete/merge smoke test");
    Expect(env.index->Search(Value(static_cast<int32_t>(200))).empty(),
           "Deleted key should be absent after delete/merge smoke test");
}

void TestConcurrentSplitAndScanSmoke() {
    std::string dir = "/tmp/simpledb_split_scan_smoke";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    TestEnv env(dir, false);

    {
        auto txn = env.txn_manager.Begin();
        for (int i = 1; i <= 250; ++i) {
            env.InsertRow(txn, i, i);
        }
        Expect(env.txn_manager.Commit(txn), "seed commit for split smoke failed");
    }

    auto reader = std::async(std::launch::async, [&] {
        BTreeIndexIterator it(env.index.get());
        int count = 0;
        int last = 0;
        while (it.HasNext()) {
            try {
                auto [key, rid] = it.Next();
                int current = key.AsInt32();
                Expect(current >= last, "Iterator order should remain nondecreasing during concurrent splits");
                last = current;
                (void)rid;
                ++count;
            } catch (const std::runtime_error &ex) {
                std::string msg = ex.what();
                if (msg.find("at end") != std::string::npos) break;
                throw;
            }
        }
        return count;
    });

    auto writer = std::async(std::launch::async, [&] {
        auto txn = env.txn_manager.Begin();
        for (int i = 251; i <= 500; ++i) {
            env.InsertRow(txn, i, i);
        }
        return env.txn_manager.Commit(txn);
    });

    int reader_count = reader.get();
    bool writer_ok = writer.get();
    Expect(reader_count >= 250, "Reader should traverse the pre-existing tree without crashing");
    Expect(writer_ok, "Writer commit failed during concurrent split smoke test");
    Expect(env.index->Search(Value(static_cast<int32_t>(500))).size() == 1,
           "Final inserted key should be searchable after concurrent split smoke test");
}


void TestLehmanSplitMergeScanAbortStress() {
    std::string dir = "/tmp/simpledb_lehman_stage_split_abort";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    TestEnv env(dir, false);

    {
        auto txn = env.txn_manager.Begin();
        for (int i = 1; i <= 250; ++i) env.InsertRow(txn, i, i);
        Expect(env.txn_manager.Commit(txn), "split-abort stage seed commit failed");
    }

    auto reader_loop = [&](int rounds) {
        for (int round = 0; round < rounds; ++round) {
            BTreeIndexIterator it(env.index.get());
            while (it.HasNext()) {
                try {
                    auto [key, rid] = it.Next();
                    (void)key;
                    (void)rid;
                } catch (const std::runtime_error &ex) {
                    std::string msg = ex.what();
                    if (msg.find("at end") != std::string::npos) break;
                    throw;
                }
            }
            std::this_thread::yield();
        }
    };

    auto expect_ready = [](auto &future, const std::string &message) {
        auto status = future.wait_for(std::chrono::seconds(15));
        Expect(status == std::future_status::ready, message);
        future.get();
    };

    std::promise<void> start;
    std::shared_future<void> go(start.get_future());
    auto reader1 = std::async(std::launch::async, [&] { go.wait(); reader_loop(6); });
    auto reader2 = std::async(std::launch::async, [&] { go.wait(); reader_loop(6); });
    auto committer = std::async(std::launch::async, [&] {
        go.wait();
        for (int base = 251; base <= 314; base += 8) {
            auto txn = env.txn_manager.Begin();
            for (int key = base; key < base + 8; ++key) env.InsertRow(txn, key, key);
            Expect(env.txn_manager.Commit(txn), "split-abort stage commit failed");
        }
    });
    auto aborter = std::async(std::launch::async, [&] {
        go.wait();
        for (int base = 1000; base <= 1064; base += 8) {
            auto txn = env.txn_manager.Begin();
            for (int key = base; key < base + 8; ++key) env.InsertRow(txn, key, key);
            env.txn_manager.Abort(txn);
        }
    });
    start.set_value();

    expect_ready(reader1, "split-abort stage reader1 hung");
    expect_ready(reader2, "split-abort stage reader2 hung");
    expect_ready(committer, "split-abort stage committer hung");
    expect_ready(aborter, "split-abort stage aborter hung");


    std::string error;
    bool ok = ConsistencyChecker::VerifyIndexAgainstHeap(*env.heap, *env.index, 0, &error);
    if (!ok) {
        std::cerr << "Lehman stress verification failed: " << error << "\n";
        Tuple tuple;
        RID probe{1, 0};
        if (env.heap->GetTuple(probe, &tuple)) {
            std::cerr << "heap[1,0] key=" << tuple.GetValue(0).AsInt32() << "\n";
        }
        auto hits = env.index->Search(Value(static_cast<int32_t>(1)));
        std::cerr << "search(1) hits=" << hits.size() << "\n";
        BTreeIndexIterator it(env.index.get());
        int seen = 0;
        while (it.HasNext() && seen < 12) {
            auto [key, rid] = it.Next();
            std::cerr << "iter key=" << key.AsInt32() << " rid=" << rid.page_no << ":" << rid.slot_no << "\n";
            ++seen;
        }
        std::string shape_error;
        bool shape_ok = ConsistencyChecker::VerifyBTreeStructure(*env.index, &shape_error);
        std::cerr << "shape_ok=" << shape_ok << " shape_error=" << shape_error << "\n";
    }
    Expect(ok, error);
}


void TestRandomizedSeededConcurrencyHarness() {
    constexpr int kSeeds = 5;
    for (int seed = 1; seed <= kSeeds; ++seed) {
        try {
            std::string dir = "/tmp/simpledb_random_concurrency_" + std::to_string(seed);
            std::filesystem::remove_all(dir);
            std::filesystem::create_directories(dir);
            TestEnv env(dir, false);

            std::mt19937 gen(static_cast<uint32_t>(seed * 1237));
            std::unordered_map<int, RID> committed_rows;
            int next_insert_key = 1;

            auto pick_existing = [&](int *out_key, RID *out_rid) -> bool {
                if (committed_rows.empty()) return false;
                std::size_t offset = static_cast<std::size_t>(gen()) % committed_rows.size();
                auto it = committed_rows.begin();
                std::advance(it, static_cast<long>(offset));
                *out_key = it->first;
                *out_rid = it->second;
                return true;
            };

            for (int step = 0; step < 240; ++step) {
                int dice = static_cast<int>(gen() % 100);
                if (dice < 42) {
                    int key = (step < 120 || next_insert_key < 32)
                                  ? next_insert_key++
                                  : 1 + static_cast<int>(gen() % std::max(1, next_insert_key - 1));
                    auto txn = env.txn_manager.Begin();
                    try {
                        RID rid = env.InsertRow(txn, key, key * 10 + seed);
                        if (gen() % 5 == 0) {
                            env.txn_manager.Abort(txn);
                        } else {
                            Expect(env.txn_manager.Commit(txn), "seeded random insert commit failed");
                            committed_rows[key] = rid;
                        }
                    } catch (...) {
                        env.txn_manager.Abort(txn);
                    }
                } else if (dice < 68) {
                    int key = 0;
                    RID rid{};
                    if (!pick_existing(&key, &rid)) continue;
                    auto txn = env.txn_manager.Begin();
                    try {
                        bool ok = env.table->DeleteTuple(txn, rid);
                        if (!ok || gen() % 4 == 0) {
                            env.txn_manager.Abort(txn);
                        } else {
                            Expect(env.txn_manager.Commit(txn), "seeded random delete commit failed");
                            auto it = committed_rows.find(key);
                            if (it != committed_rows.end() && it->second == rid) committed_rows.erase(it);
                        }
                    } catch (...) {
                        env.txn_manager.Abort(txn);
                    }
                } else if (dice < 84) {
                    int probe_key = 1 + static_cast<int>(gen() % std::max(1, next_insert_key + 8));
                    (void)env.index->Search(Value(static_cast<int32_t>(probe_key)));
                } else {
                    int lower = 1 + static_cast<int>(gen() % std::max(1, next_insert_key + 4));
                    BTreeIndexIterator it(env.index.get(), Value(static_cast<int32_t>(lower)));
                    int last = std::numeric_limits<int>::min();
                    int seen = 0;
                    while (it.HasNext() && seen < 80) {
                        auto [key, rid] = it.Next();
                        int current = key.AsInt32();
                        Expect(current >= last, "seeded random iterator order regression");
                        last = current;
                        (void)rid;
                        ++seen;
                    }
                }

                if (step % 40 == 0) {
                    std::string error;
                    Expect(ConsistencyChecker::VerifyHeapReadable(*env.heap, &error), error);
                    Expect(ConsistencyChecker::VerifyIndexAgainstHeap(*env.heap, *env.index, 0, &error), error);
                    Expect(ConsistencyChecker::VerifyBTreeStructure(*env.index, &error), error);
                }
            }

            std::string error;
            Expect(ConsistencyChecker::VerifyHeapReadable(*env.heap, &error), error);
            Expect(ConsistencyChecker::VerifyIndexAgainstHeap(*env.heap, *env.index, 0, &error), error);
            Expect(ConsistencyChecker::VerifyBTreeStructure(*env.index, &error), error);
        } catch (const std::exception &ex) {
            throw std::runtime_error(std::string("randomized concurrency seed ") + std::to_string(seed) + " failed: " + ex.what());
        }
    }
}



void TestUnsafeNoTxnWritePathsRejectedWhenLockingEnabled() {
    std::string dir = "/tmp/simpledb_no_txn_guard";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    TestEnv env(dir, false);

    bool table_guarded = false;
    try {
        env.table->InsertTuple(Tuple({Value(static_cast<int32_t>(1)), Value(static_cast<int32_t>(10))}));
    } catch (const std::exception &ex) {
        table_guarded = std::string(ex.what()).find("without a transaction is disabled") != std::string::npos;
    }
    Expect(table_guarded, "Table non-transactional write path should be rejected when locking is enabled");

    bool heap_guarded = false;
    try {
        env.heap->InsertTuple(Tuple({Value(static_cast<int32_t>(1)), Value(static_cast<int32_t>(10))}));
    } catch (const std::exception &ex) {
        heap_guarded = std::string(ex.what()).find("without a transaction is disabled") != std::string::npos;
    }
    Expect(heap_guarded, "Heap non-transactional write path should be rejected when locking is enabled");
}

void TestLehmanMixedProbeScanMergeStress() {
    std::string dir = "/tmp/simpledb_lehman_mixed_probe_scan_merge";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    TestEnv env(dir, false);

    {
        auto txn = env.txn_manager.Begin();
        for (int i = 1; i <= 500; ++i) env.InsertRow(txn, i, i * 10);
        Expect(env.txn_manager.Commit(txn), "mixed stress seed commit failed");
    }

    std::promise<void> start;
    std::shared_future<void> go(start.get_future());

    auto scan_reader = std::async(std::launch::async, [&] {
        go.wait();
        for (int round = 0; round < 16; ++round) {
            BTreeIndexIterator it(env.index.get(), Value(static_cast<int32_t>(120)));
            int seen = 0;
            while (it.HasNext() && seen < 260) {
                try {
                    auto [key, rid] = it.Next();
                    (void)key;
                    (void)rid;
                    ++seen;
                } catch (const std::runtime_error &ex) {
                    if (std::string(ex.what()).find("at end") != std::string::npos) break;
                    throw;
                }
            }
            std::this_thread::yield();
        }
    });

    auto full_reader = std::async(std::launch::async, [&] {
        go.wait();
        for (int round = 0; round < 10; ++round) {
            BTreeIndexIterator it(env.index.get());
            int seen = 0;
            while (it.HasNext() && seen < 520) {
                try {
                    auto [key, rid] = it.Next();
                    (void)key;
                    (void)rid;
                    ++seen;
                } catch (const std::runtime_error &ex) {
                    if (std::string(ex.what()).find("at end") != std::string::npos) break;
                    throw;
                }
            }
            std::this_thread::yield();
        }
    });

    auto exact_prober = std::async(std::launch::async, [&] {
        go.wait();
        std::mt19937 gen(20260410u);
        for (int round = 0; round < 2500; ++round) {
            int probe_key = 100 + static_cast<int>(gen() % 280);
            auto hits = env.index->Search(Value(static_cast<int32_t>(probe_key)));
            for (const RID &rid : hits) {
                Tuple tuple;
                if (env.heap->GetTuple(rid, &tuple)) {
                    Expect(tuple.GetValue(0).AsInt32() == probe_key,
                           "mixed stress exact probe returned RID whose heap tuple key mismatched");
                }
            }
            if (round % 25 == 0) std::this_thread::yield();
        }
    });

    auto writer = std::async(std::launch::async, [&] {
        go.wait();
        for (int round = 0; round < 8; ++round) {
            {
                auto txn = env.txn_manager.Begin();
                for (int key = 150; key <= 340; ++key) {
                    auto hits = env.index->Search(Value(static_cast<int32_t>(key)));
                    for (const RID &rid : hits) {
                        env.table->DeleteTuple(txn, rid);
                    }
                }
                Expect(env.txn_manager.Commit(txn), "mixed stress delete phase commit failed");
            }

            {
                auto txn = env.txn_manager.Begin();
                for (int key = 150; key <= 340; ++key) {
                    env.InsertRow(txn, key, key * 100 + round);
                }
                Expect(env.txn_manager.Commit(txn), "mixed stress reinsert phase commit failed");
            }

            {
                auto txn = env.txn_manager.Begin();
                for (int key = 1000 + round * 32; key < 1000 + round * 32 + 32; ++key) {
                    env.InsertRow(txn, key, key);
                }
                Expect(env.txn_manager.Abort(txn), "mixed stress abort phase failed");
            }
        }
    });

    start.set_value();
    scan_reader.get();
    full_reader.get();
    exact_prober.get();
    writer.get();

    std::string error;
    Expect(ConsistencyChecker::VerifyHeapReadable(*env.heap, &error), error);
    Expect(ConsistencyChecker::VerifyIndexAgainstHeap(*env.heap, *env.index, 0, &error), error);
    Expect(ConsistencyChecker::VerifyBTreeStructure(*env.index, &error), error);
}

}  // namespace

int main(int argc, char **argv) {
    try {
        std::cerr << "Running TestSingleUpgraderRule...\n";
        TestSingleUpgraderRule();
        std::cerr << "Running TestDeadlockDetection...\n";
        TestDeadlockDetection();
        std::cerr << "Running TestDeadlockVictimHoldsLocksUntilRollbackCompletes...\n";
        TestDeadlockVictimHoldsLocksUntilRollbackCompletes();
        std::cerr << "Running TestThreeWayDeadlockDetection...\n";
        TestThreeWayDeadlockDetection();
        std::cerr << "Running TestPhantomPreventionOnIndexedRange...\n";
        TestPhantomPreventionOnIndexedRange();
        std::cerr << "Running TestConcurrentUniqueInsertRace...\n";
        TestConcurrentUniqueInsertRace();
        std::cerr << "Running TestAbortAfterSplitUndo...\n";
        TestAbortAfterSplitUndo();
        std::cerr << "Running TestUnsafeNoTxnWritePathsRejectedWhenLockingEnabled...\n";
        TestUnsafeNoTxnWritePathsRejectedWhenLockingEnabled();
        std::cerr << "Running TestDeleteMergeAndScanSmoke...\n";
        TestDeleteMergeAndScanSmoke();
        std::cerr << "Running TestConcurrentSplitAndScanSmoke...\n";
        TestConcurrentSplitAndScanSmoke();
        std::cerr << "Running TestLehmanSplitMergeScanAbortStress...\n";
        TestLehmanSplitMergeScanAbortStress();
        std::cerr << "Running TestLehmanMixedProbeScanMergeStress...\n";
        TestLehmanMixedProbeScanMergeStress();
        std::cerr << "Running TestRandomizedSeededConcurrencyHarness...\n";
        TestRandomizedSeededConcurrencyHarness();
        std::cout << "Concurrency regression suite passed.\n";
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "Concurrency regression suite failed: " << ex.what() << "\n";
        return 1;
    }
}
