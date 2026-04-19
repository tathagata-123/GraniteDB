#include "lock_manager_helpers.h"

#include <algorithm>
#include <chrono>
#include <functional>

namespace simpledb {
using namespace lock_manager_helpers;

// Deadlock handling is intentionally separated from normal lock acquisition.
// The runtime path should read like lock protocol code; the background detector
// should read like graph analysis code.

std::unordered_map<TxnId, std::unordered_set<TxnId>>
LockManager::BuildWaitsForGraphLocked() const {
    std::unordered_map<TxnId, std::unordered_set<TxnId>> graph;

    auto process_queue = [&](const LockRequestQueue &queue, bool is_table_lock) {
        for (auto it = queue.requests.begin(); it != queue.requests.end(); ++it) {
            if (it->granted) continue;
            graph[it->txn_id];

            for (auto prev = queue.requests.begin(); prev != it; ++prev) {
                if (prev->txn_id == it->txn_id) continue;
                if (!prev->granted) {
                    graph[it->txn_id].insert(prev->txn_id);
                    continue;
                }
                if (!Compatible(it->mode, prev->mode, is_table_lock)) {
                    graph[it->txn_id].insert(prev->txn_id);
                }
            }

            for (auto later = std::next(it); later != queue.requests.end(); ++later) {
                if (later->granted && later->txn_id != it->txn_id &&
                    !Compatible(it->mode, later->mode, is_table_lock)) {
                    graph[it->txn_id].insert(later->txn_id);
                }
            }
        }
    };

    auto process_range_queue = [&](const KeyRangeLockRequestQueue &queue) {
        for (auto it = queue.requests.begin(); it != queue.requests.end(); ++it) {
            if (it->granted) continue;
            graph[it->txn_id];

            for (auto prev = queue.requests.begin(); prev != it; ++prev) {
                if (prev->txn_id == it->txn_id) continue;
                if (!RangesOverlap(*prev, *it)) continue;
                if (!prev->granted) {
                    graph[it->txn_id].insert(prev->txn_id);
                    continue;
                }
                if (!RangeModesCompatible(it->mode, prev->mode)) {
                    graph[it->txn_id].insert(prev->txn_id);
                }
            }

            for (auto later = std::next(it); later != queue.requests.end(); ++later) {
                if (later->txn_id == it->txn_id || !later->granted) continue;
                if (!RangesOverlap(*later, *it)) continue;
                if (!RangeModesCompatible(it->mode, later->mode)) {
                    graph[it->txn_id].insert(later->txn_id);
                }
            }
        }
    };

    for (const auto &entry : table_lock_table_) process_queue(entry.second, true);
    for (const auto &entry : record_lock_table_) process_queue(entry.second, false);
    for (const auto &entry : key_range_lock_table_) process_range_queue(entry.second);

    return graph;
}

std::optional<TxnId> LockManager::DetectCycleVictimLocked(
    const std::unordered_map<TxnId, std::unordered_set<TxnId>> &graph) const {
    std::unordered_map<TxnId, int> state;
    std::vector<TxnId> stack;
    std::optional<TxnId> victim;

    std::function<void(TxnId)> dfs = [&](TxnId u) {
        if (victim.has_value()) return;
        state[u] = 1;
        stack.push_back(u);

        auto it = graph.find(u);
        if (it != graph.end()) {
            for (TxnId v : it->second) {
                if (state[v] == 0) {
                    dfs(v);
                    if (victim.has_value()) return;
                } else if (state[v] == 1) {
                    TxnId chosen = v;
                    auto cycle_begin = std::find(stack.begin(), stack.end(), v);
                    for (auto iter = cycle_begin; iter != stack.end(); ++iter) {
                        chosen = std::max(chosen, *iter);
                    }
                    victim = chosen;
                    return;
                }
            }
        }

        stack.pop_back();
        state[u] = 2;
    };

    for (const auto &entry : graph) {
        if (state[entry.first] == 0) {
            dfs(entry.first);
            if (victim.has_value()) return victim;
        }
    }
    return std::nullopt;
}

void LockManager::AbortVictimLocked(TxnId victim_txn_id) {
    auto it = txn_table_.find(victim_txn_id);
    if (it == txn_table_.end()) return;
    TransactionPtr victim = it->second;

    TransactionState st = victim->GetState();
    if (st == TransactionState::COMMITTED || st == TransactionState::ABORTED ||
        st == TransactionState::ABORTING) {
        return;
    }

    victim->TryMarkAborting();
    RemoveUngrantedRequestsLocked(victim_txn_id);
}

void LockManager::DeadlockWorker() {
    while (!stop_detector_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(deadlock_check_interval_ms_));

        std::vector<TransactionPtr> victims;
        {
            std::lock_guard<std::mutex> guard(latch_);
            auto graph = BuildWaitsForGraphLocked();
            while (true) {
                auto victim = DetectCycleVictimLocked(graph);
                if (!victim.has_value()) break;
                auto it = txn_table_.find(*victim);
                TransactionPtr victim_txn = (it != txn_table_.end()) ? it->second : nullptr;
                AbortVictimLocked(*victim);
                if (victim_txn != nullptr) victims.push_back(victim_txn);
                graph = BuildWaitsForGraphLocked();
            }
        }

        for (const auto &victim_txn : victims) {
            std::function<void(const TransactionPtr &)> cb;
            {
                std::lock_guard<std::mutex> guard(latch_);
                cb = abort_callback_;
            }
            if (cb) cb(victim_txn);
        }
    }
}
}  // namespace simpledb
