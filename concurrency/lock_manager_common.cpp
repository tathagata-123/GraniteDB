#include "lock_manager_helpers.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <stdexcept>

namespace simpledb {
using namespace lock_manager_helpers;

// Basic compatibility, queue search, and grant checks live here. These are the
// reusable building blocks used by the higher-level lock acquisition paths.

LockManager::LockManager(uint32_t deadlock_check_interval_ms)
    : deadlock_check_interval_ms_(deadlock_check_interval_ms) {
    deadlock_thread_ = std::thread(&LockManager::DeadlockWorker, this);
}

LockManager::~LockManager() {
    stop_detector_.store(true);
    if (deadlock_thread_.joinable()) {
        deadlock_thread_.join();
    }
}

void LockManager::RegisterTransaction(const TransactionPtr &txn) {
    std::lock_guard<std::mutex> guard(latch_);
    txn_table_[txn->GetTransactionId()] = txn;
}

void LockManager::UnregisterTransaction(TxnId txn_id) {
    std::lock_guard<std::mutex> guard(latch_);
    txn_table_.erase(txn_id);
}

void LockManager::SetAbortCallback(std::function<void(const TransactionPtr &)> abort_callback) {
    std::lock_guard<std::mutex> guard(latch_);
    abort_callback_ = std::move(abort_callback);
}

bool LockManager::Compatible(LockMode requested, LockMode held, bool is_table_lock) const {
    if (!is_table_lock) {
        return requested == LockMode::S && held == LockMode::S;
    }

    switch (requested) {
        case LockMode::IS:
            return held == LockMode::IS || held == LockMode::IX || held == LockMode::S || held == LockMode::SIX;
        case LockMode::IX:
            return held == LockMode::IS || held == LockMode::IX;
        case LockMode::S:
            return held == LockMode::IS || held == LockMode::S;
        case LockMode::SIX:
            return held == LockMode::IS;
        case LockMode::X:
            return false;
    }
    return false;
}

bool LockManager::RangeModesCompatible(LockMode requested, LockMode held) const {
    return requested == LockMode::S && held == LockMode::S;
}

std::list<LockRequest>::iterator LockManager::FindRequest(LockRequestQueue &queue, TxnId txn_id) {
    for (auto it = queue.requests.begin(); it != queue.requests.end(); ++it) {
        if (it->txn_id == txn_id) return it;
    }
    return queue.requests.end();
}

std::list<LockRequest>::const_iterator LockManager::FindRequest(const LockRequestQueue &queue, TxnId txn_id) const {
    for (auto it = queue.requests.begin(); it != queue.requests.end(); ++it) {
        if (it->txn_id == txn_id) return it;
    }
    return queue.requests.end();
}

std::list<KeyRangeLockRequest>::iterator LockManager::FindKeyRangeRequest(
    KeyRangeLockRequestQueue &queue,
    TxnId txn_id,
    const std::optional<std::vector<Value>> &lower_key,
    const std::optional<std::vector<Value>> &upper_key,
    bool lower_inclusive,
    bool upper_inclusive) {
    for (auto it = queue.requests.begin(); it != queue.requests.end(); ++it) {
        if (it->txn_id != txn_id) continue;
        if (it->lower_unbounded != !lower_key.has_value() || it->upper_unbounded != !upper_key.has_value()) continue;
        if (it->lower_inclusive != lower_inclusive || it->upper_inclusive != upper_inclusive) continue;
        std::optional<std::vector<Value>> existing_lower = it->lower_unbounded ? std::nullopt : std::optional<std::vector<Value>>(it->lower_key);
        std::optional<std::vector<Value>> existing_upper = it->upper_unbounded ? std::nullopt : std::optional<std::vector<Value>>(it->upper_key);
        if (SameOptionalKey(existing_lower, lower_key) && SameOptionalKey(existing_upper, upper_key)) {
            return it;
        }
    }
    return queue.requests.end();
}

std::list<KeyRangeLockRequest>::const_iterator LockManager::FindKeyRangeRequest(
    const KeyRangeLockRequestQueue &queue,
    TxnId txn_id,
    const std::optional<std::vector<Value>> &lower_key,
    const std::optional<std::vector<Value>> &upper_key,
    bool lower_inclusive,
    bool upper_inclusive) const {
    for (auto it = queue.requests.begin(); it != queue.requests.end(); ++it) {
        if (it->txn_id != txn_id) continue;
        if (it->lower_unbounded != !lower_key.has_value() || it->upper_unbounded != !upper_key.has_value()) continue;
        if (it->lower_inclusive != lower_inclusive || it->upper_inclusive != upper_inclusive) continue;
        std::optional<std::vector<Value>> existing_lower = it->lower_unbounded ? std::nullopt : std::optional<std::vector<Value>>(it->lower_key);
        std::optional<std::vector<Value>> existing_upper = it->upper_unbounded ? std::nullopt : std::optional<std::vector<Value>>(it->upper_key);
        if (SameOptionalKey(existing_lower, lower_key) && SameOptionalKey(existing_upper, upper_key)) {
            return it;
        }
    }
    return queue.requests.end();
}

bool LockManager::CanGrantRequest(const LockRequestQueue &queue,
                                  std::list<LockRequest>::const_iterator target_it,
                                  bool is_table_lock) const {
    for (auto it = queue.requests.begin(); it != target_it; ++it) {
        if (it->txn_id == target_it->txn_id) continue;
        if (!it->granted) {
            return false;
        }
        if (!Compatible(target_it->mode, it->mode, is_table_lock)) {
            return false;
        }
    }

    for (auto it = std::next(target_it); it != queue.requests.end(); ++it) {
        if (!it->granted) continue;
        if (it->txn_id == target_it->txn_id) continue;
        if (!Compatible(target_it->mode, it->mode, is_table_lock)) {
            return false;
        }
    }
    return true;
}

bool LockManager::CanGrantKeyRangeRequest(const KeyRangeLockRequestQueue &queue,
                                          std::list<KeyRangeLockRequest>::const_iterator target_it) const {
    for (auto it = queue.requests.begin(); it != target_it; ++it) {
        if (it->txn_id == target_it->txn_id) continue;
        if (!RangesOverlap(*it, *target_it)) continue;
        if (!it->granted) {
            return false;
        }
        if (!RangeModesCompatible(target_it->mode, it->mode)) {
            return false;
        }
    }

    for (auto it = std::next(target_it); it != queue.requests.end(); ++it) {
        if (it->txn_id == target_it->txn_id) continue;
        if (!it->granted) continue;
        if (!RangesOverlap(*it, *target_it)) continue;
        if (!RangeModesCompatible(target_it->mode, it->mode)) {
            return false;
        }
    }
    return true;
}
}  // namespace simpledb
