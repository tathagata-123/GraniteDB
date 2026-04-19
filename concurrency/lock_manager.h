#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "transaction.h"
#include "../common/value.h"

namespace simpledb {

struct LockRequest {
    TxnId txn_id{0};
    LockMode mode{LockMode::IS};
    bool granted{false};
};

struct LockRequestQueue {
    std::list<LockRequest> requests;
    std::condition_variable_any cv;
    std::optional<TxnId> upgrading_txn;
};

struct KeyRangeLockRequest {
    TxnId txn_id{0};
    LockMode mode{LockMode::S};
    std::vector<Value> lower_key;
    std::vector<Value> upper_key;
    bool lower_unbounded{true};
    bool upper_unbounded{true};
    bool lower_inclusive{true};
    bool upper_inclusive{true};
    bool granted{false};
};

struct KeyRangeLockRequestQueue {
    std::list<KeyRangeLockRequest> requests;
    std::condition_variable_any cv;
    std::optional<TxnId> upgrading_txn;
};

class LockManager {
public:
    explicit LockManager(uint32_t deadlock_check_interval_ms = 200);
    ~LockManager();

    void RegisterTransaction(const TransactionPtr &txn);
    void UnregisterTransaction(TxnId txn_id);
    void SetAbortCallback(std::function<void(const TransactionPtr &)> abort_callback);

    bool LockTable(const TransactionPtr &txn, RelationId relation_id, LockMode mode);
    bool LockRecord(const TransactionPtr &txn, const RecordLockId &record_id, LockMode mode);
    bool LockKey(const TransactionPtr &txn,
                 RelationId index_relation_id,
                 const std::vector<Value> &key,
                 LockMode mode);
    bool LockKeyRange(const TransactionPtr &txn,
                      RelationId index_relation_id,
                      std::optional<std::vector<Value>> lower_key,
                      std::optional<std::vector<Value>> upper_key,
                      bool lower_inclusive,
                      bool upper_inclusive,
                      LockMode mode);

    void ReleaseAll(const TransactionPtr &txn);

    void SetRecordLockEscalationThreshold(std::size_t threshold) {
        std::lock_guard<std::mutex> guard(latch_);
        record_lock_escalation_threshold_ = threshold;
    }

private:
    bool Compatible(LockMode requested, LockMode held, bool is_table_lock) const;
    bool RangeModesCompatible(LockMode requested, LockMode held) const;
    bool CanGrantRequest(const LockRequestQueue &queue,
                         std::list<LockRequest>::const_iterator target_it,
                         bool is_table_lock) const;
    bool CanGrantKeyRangeRequest(const KeyRangeLockRequestQueue &queue,
                                 std::list<KeyRangeLockRequest>::const_iterator target_it) const;

    std::list<LockRequest>::iterator FindRequest(LockRequestQueue &queue, TxnId txn_id);
    std::list<LockRequest>::const_iterator FindRequest(const LockRequestQueue &queue, TxnId txn_id) const;

    std::list<KeyRangeLockRequest>::iterator FindKeyRangeRequest(
        KeyRangeLockRequestQueue &queue,
        TxnId txn_id,
        const std::optional<std::vector<Value>> &lower_key,
        const std::optional<std::vector<Value>> &upper_key,
        bool lower_inclusive,
        bool upper_inclusive);
    std::list<KeyRangeLockRequest>::const_iterator FindKeyRangeRequest(
        const KeyRangeLockRequestQueue &queue,
        TxnId txn_id,
        const std::optional<std::vector<Value>> &lower_key,
        const std::optional<std::vector<Value>> &upper_key,
        bool lower_inclusive,
        bool upper_inclusive) const;

    void DeadlockWorker();
    std::unordered_map<TxnId, std::unordered_set<TxnId>> BuildWaitsForGraphLocked() const;
    std::optional<TxnId> DetectCycleVictimLocked(
        const std::unordered_map<TxnId, std::unordered_set<TxnId>> &graph) const;
    void AbortVictimLocked(TxnId victim_txn_id);
    void RemoveUngrantedRequestsLocked(TxnId txn_id);

    void MaybeEscalateRecordLocks(const TransactionPtr &txn, RelationId relation_id, LockMode mode);

private:
    mutable std::mutex latch_;

    std::unordered_map<RelationId, LockRequestQueue> table_lock_table_;
    std::unordered_map<RecordLockId, LockRequestQueue, RecordLockIdHash> record_lock_table_;
    std::unordered_map<RelationId, KeyRangeLockRequestQueue> key_range_lock_table_;
    std::unordered_map<TxnId, TransactionPtr> txn_table_;

    std::atomic<bool> stop_detector_{false};
    std::thread deadlock_thread_;
    uint32_t deadlock_check_interval_ms_;
    std::function<void(const TransactionPtr &)> abort_callback_;
    std::size_t record_lock_escalation_threshold_{0};
};

}  // namespace simpledb
