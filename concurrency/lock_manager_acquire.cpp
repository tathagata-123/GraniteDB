#include "lock_manager_helpers.h"

#include <stdexcept>

namespace simpledb {
using namespace lock_manager_helpers;

// Blocking lock acquisition for table, record, key, and key-range locks.

bool LockManager::LockTable(const TransactionPtr &txn, RelationId relation_id, LockMode mode) {
    if (!IsTableLockMode(mode)) {
        throw std::runtime_error("Invalid table lock mode");
    }
    if (txn->IsAbortPendingOrDone()) return false;

    std::optional<LockMode> held = txn->GetTableLockMode(relation_id);
    if (held.has_value() && TableModeSatisfies(*held, mode)) return true;
    LockMode target_mode = held.has_value() ? CombineTableModes(*held, mode) : mode;

    std::unique_lock<std::mutex> lk(latch_);
    LockRequestQueue &queue = table_lock_table_[relation_id];

    auto existing = FindRequest(queue, txn->GetTransactionId());
    if (existing == queue.requests.end()) {
        queue.requests.push_back(LockRequest{txn->GetTransactionId(), target_mode, false});
    } else {
        if (existing->granted && existing->mode != target_mode) {
            if (queue.upgrading_txn.has_value() && *queue.upgrading_txn != txn->GetTransactionId()) {
                txn->TryMarkAborting();
                return false;
            }
            queue.upgrading_txn = txn->GetTransactionId();
            existing->mode = target_mode;
            existing->granted = false;
        } else {
            existing->mode = target_mode;
        }
    }

    while (true) {
        if (txn->IsAbortPendingOrDone()) {
            auto it = FindRequest(queue, txn->GetTransactionId());
            if (it != queue.requests.end()) {
                queue.requests.erase(it);
                if (queue.upgrading_txn == txn->GetTransactionId()) queue.upgrading_txn.reset();
                queue.cv.notify_all();
            }
            return false;
        }

        auto it = FindRequest(queue, txn->GetTransactionId());
        if (it == queue.requests.end()) return false;

        if (CanGrantRequest(queue, it, true)) {
            it->granted = true;
            if (queue.upgrading_txn == txn->GetTransactionId()) queue.upgrading_txn.reset();
            txn->SetActiveIfNotAborting();
            txn->AddTableLock(relation_id, it->mode);
            return true;
        }

        txn->SetWaitingIfNotAborting();
        queue.cv.wait(lk);
    }
}

bool LockManager::LockRecord(const TransactionPtr &txn,
                             const RecordLockId &record_id,
                             LockMode mode) {
    if (!IsTupleOrRangeLockMode(mode)) {
        throw std::runtime_error("Record locks only support S and X");
    }
    if (txn->IsAbortPendingOrDone()) return false;

    if (mode == LockMode::S) {
        if (!(txn->HoldsTableIS(record_id.relation_id) || txn->HoldsTableS(record_id.relation_id))) {
            throw std::runtime_error("Record S lock requires table IS/IX/S/SIX/X first");
        }
    }
    if (mode == LockMode::X) {
        if (!(txn->HoldsTableIX(record_id.relation_id) || txn->HoldsTableSIX(record_id.relation_id) || txn->HoldsTableX(record_id.relation_id))) {
            throw std::runtime_error("Record X lock requires table IX/SIX/X first");
        }
    }

    std::optional<LockMode> held = txn->GetRecordLockMode(record_id);
    if (held.has_value() && TupleOrRangeModeSatisfies(*held, mode)) return true;
    LockMode target_mode = held.has_value() ? CombineTupleOrRangeModes(*held, mode) : mode;

    std::unique_lock<std::mutex> lk(latch_);
    LockRequestQueue &queue = record_lock_table_[record_id];

    auto existing = FindRequest(queue, txn->GetTransactionId());
    if (existing == queue.requests.end()) {
        queue.requests.push_back(LockRequest{txn->GetTransactionId(), target_mode, false});
    } else {
        if (existing->granted && existing->mode != target_mode) {
            if (queue.upgrading_txn.has_value() && *queue.upgrading_txn != txn->GetTransactionId()) {
                txn->TryMarkAborting();
                return false;
            }
            queue.upgrading_txn = txn->GetTransactionId();
            existing->mode = target_mode;
            existing->granted = false;
        } else {
            existing->mode = target_mode;
        }
    }

    while (true) {
        if (txn->IsAbortPendingOrDone()) {
            auto it = FindRequest(queue, txn->GetTransactionId());
            if (it != queue.requests.end()) {
                queue.requests.erase(it);
                if (queue.upgrading_txn == txn->GetTransactionId()) queue.upgrading_txn.reset();
                queue.cv.notify_all();
            }
            return false;
        }

        auto it = FindRequest(queue, txn->GetTransactionId());
        if (it == queue.requests.end()) return false;

        if (CanGrantRequest(queue, it, false)) {
            it->granted = true;
            if (queue.upgrading_txn == txn->GetTransactionId()) queue.upgrading_txn.reset();
            txn->SetActiveIfNotAborting();
            txn->AddRecordLock(record_id, it->mode);
            lk.unlock();
            MaybeEscalateRecordLocks(txn, record_id.relation_id, it->mode);
            return true;
        }

        txn->SetWaitingIfNotAborting();
        queue.cv.wait(lk);
    }
}

bool LockManager::LockKey(const TransactionPtr &txn,
                          RelationId index_relation_id,
                          const std::vector<Value> &key,
                          LockMode mode) {
    return LockKeyRange(txn,
                        index_relation_id,
                        key,
                        key,
                        true,
                        true,
                        mode);
}

bool LockManager::LockKeyRange(const TransactionPtr &txn,
                               RelationId index_relation_id,
                               std::optional<std::vector<Value>> lower_key,
                               std::optional<std::vector<Value>> upper_key,
                               bool lower_inclusive,
                               bool upper_inclusive,
                               LockMode mode) {
    if (!IsTupleOrRangeLockMode(mode)) {
        throw std::runtime_error("Key-range locks only support S and X");
    }
    if (txn->IsAbortPendingOrDone()) return false;

    std::unique_lock<std::mutex> lk(latch_);
    KeyRangeLockRequestQueue &queue = key_range_lock_table_[index_relation_id];
    auto existing = FindKeyRangeRequest(queue,
                                        txn->GetTransactionId(),
                                        lower_key,
                                        upper_key,
                                        lower_inclusive,
                                        upper_inclusive);
    if (existing == queue.requests.end()) {
        KeyRangeLockRequest req;
        req.txn_id = txn->GetTransactionId();
        req.mode = mode;
        req.lower_unbounded = !lower_key.has_value();
        req.upper_unbounded = !upper_key.has_value();
        req.lower_inclusive = lower_inclusive;
        req.upper_inclusive = upper_inclusive;
        if (lower_key.has_value()) req.lower_key = *lower_key;
        if (upper_key.has_value()) req.upper_key = *upper_key;
        queue.requests.push_back(std::move(req));
    } else {
        LockMode target_mode = CombineTupleOrRangeModes(existing->mode, mode);
        if (existing->granted && existing->mode != target_mode) {
            if (queue.upgrading_txn.has_value() && *queue.upgrading_txn != txn->GetTransactionId()) {
                txn->TryMarkAborting();
                return false;
            }
            queue.upgrading_txn = txn->GetTransactionId();
            existing->mode = target_mode;
            existing->granted = false;
        } else if (existing->granted && TupleOrRangeModeSatisfies(existing->mode, mode)) {
            return true;
        } else {
            existing->mode = target_mode;
        }
    }

    while (true) {
        if (txn->IsAbortPendingOrDone()) {
            auto it = FindKeyRangeRequest(queue,
                                          txn->GetTransactionId(),
                                          lower_key,
                                          upper_key,
                                          lower_inclusive,
                                          upper_inclusive);
            if (it != queue.requests.end()) {
                queue.requests.erase(it);
                if (queue.upgrading_txn == txn->GetTransactionId()) queue.upgrading_txn.reset();
                queue.cv.notify_all();
            }
            return false;
        }

        auto it = FindKeyRangeRequest(queue,
                                      txn->GetTransactionId(),
                                      lower_key,
                                      upper_key,
                                      lower_inclusive,
                                      upper_inclusive);
        if (it == queue.requests.end()) return false;

        if (CanGrantKeyRangeRequest(queue, it)) {
            it->granted = true;
            if (queue.upgrading_txn == txn->GetTransactionId()) queue.upgrading_txn.reset();
            txn->SetActiveIfNotAborting();
            return true;
        }

        txn->SetWaitingIfNotAborting();
        queue.cv.wait(lk);
    }
}
}  // namespace simpledb
