#include "lock_manager_helpers.h"

namespace simpledb {
using namespace lock_manager_helpers;

// Releasing and cleanup are isolated here so the reader can understand exactly
// how aborted/waiting requests are removed without mentally interleaving that
// logic with the acquisition path.

void LockManager::RemoveUngrantedRequestsLocked(TxnId txn_id) {
    for (auto &entry : table_lock_table_) {
        auto &queue = entry.second;
        for (auto it = queue.requests.begin(); it != queue.requests.end();) {
            if (it->txn_id == txn_id && !it->granted) it = queue.requests.erase(it);
            else ++it;
        }
        if (queue.upgrading_txn == txn_id) queue.upgrading_txn.reset();
        queue.cv.notify_all();
    }

    for (auto &entry : record_lock_table_) {
        auto &queue = entry.second;
        for (auto it = queue.requests.begin(); it != queue.requests.end();) {
            if (it->txn_id == txn_id && !it->granted) it = queue.requests.erase(it);
            else ++it;
        }
        if (queue.upgrading_txn == txn_id) queue.upgrading_txn.reset();
        queue.cv.notify_all();
    }

    for (auto &entry : key_range_lock_table_) {
        auto &queue = entry.second;
        for (auto it = queue.requests.begin(); it != queue.requests.end();) {
            if (it->txn_id == txn_id && !it->granted) it = queue.requests.erase(it);
            else ++it;
        }
        if (queue.upgrading_txn == txn_id) queue.upgrading_txn.reset();
        queue.cv.notify_all();
    }
}

void LockManager::MaybeEscalateRecordLocks(const TransactionPtr &txn,
                                           RelationId relation_id,
                                           LockMode mode) {
    std::size_t threshold = 0;
    {
        std::lock_guard<std::mutex> guard(latch_);
        threshold = record_lock_escalation_threshold_;
    }
    if (threshold == 0) return;
    if (txn->CountRecordLocksOnRelation(relation_id) < threshold) return;

    if (mode == LockMode::S && !txn->HoldsTableS(relation_id)) {
        (void)LockTable(txn, relation_id, LockMode::S);
        return;
    }
    if (mode == LockMode::X && !txn->HoldsTableX(relation_id)) {
        (void)LockTable(txn, relation_id, LockMode::X);
    }
}

void LockManager::ReleaseAll(const TransactionPtr &txn) {
    std::lock_guard<std::mutex> guard(latch_);
    TxnId txn_id = txn->GetTransactionId();

    for (auto &entry : table_lock_table_) {
        auto &queue = entry.second;
        for (auto it = queue.requests.begin(); it != queue.requests.end();) {
            if (it->txn_id == txn_id) it = queue.requests.erase(it);
            else ++it;
        }
        if (queue.upgrading_txn == txn_id) queue.upgrading_txn.reset();
        queue.cv.notify_all();
    }

    for (auto &entry : record_lock_table_) {
        auto &queue = entry.second;
        for (auto it = queue.requests.begin(); it != queue.requests.end();) {
            if (it->txn_id == txn_id) it = queue.requests.erase(it);
            else ++it;
        }
        if (queue.upgrading_txn == txn_id) queue.upgrading_txn.reset();
        queue.cv.notify_all();
    }

    for (auto &entry : key_range_lock_table_) {
        auto &queue = entry.second;
        for (auto it = queue.requests.begin(); it != queue.requests.end();) {
            if (it->txn_id == txn_id) it = queue.requests.erase(it);
            else ++it;
        }
        if (queue.upgrading_txn == txn_id) queue.upgrading_txn.reset();
        queue.cv.notify_all();
    }

    txn->ClearAllLocks();
}
}  // namespace simpledb
