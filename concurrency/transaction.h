#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "../common/types.h"

namespace simpledb {

enum class TransactionState {
    ACTIVE,
    WAITING,
    ABORTING,
    COMMITTED,
    ABORTED
};

enum class LockMode {
    IS,
    IX,
    S,
    SIX,
    X
};

struct RecordLockId {
    RelationId relation_id{0};
    RID rid{};

    bool operator==(const RecordLockId &other) const {
        return relation_id == other.relation_id && rid == other.rid;
    }
};

struct RecordLockIdHash {
    std::size_t operator()(const RecordLockId &id) const {
        std::size_t h1 = std::hash<uint32_t>{}(id.relation_id);
        std::size_t h2 = std::hash<uint32_t>{}(id.rid.page_no);
        std::size_t h3 = std::hash<uint16_t>{}(id.rid.slot_no);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

class Transaction {
public:
    explicit Transaction(TxnId txn_id) : txn_id_(txn_id) {}

    TxnId GetTransactionId() const { return txn_id_; }

    TransactionState GetState() const {
        std::lock_guard<std::mutex> guard(latch_);
        return state_;
    }

    void SetState(TransactionState state) {
        std::lock_guard<std::mutex> guard(latch_);
        state_ = state;
        if (state_ != TransactionState::ABORTING) {
            state_cv_.notify_all();
        }
    }

    bool IsAbortPendingOrDone() const {
        std::lock_guard<std::mutex> guard(latch_);
        return state_ == TransactionState::ABORTING || state_ == TransactionState::ABORTED;
    }

    bool TryMarkAborting() {
        std::lock_guard<std::mutex> guard(latch_);
        if (state_ == TransactionState::COMMITTED || state_ == TransactionState::ABORTED) {
            return false;
        }
        if (state_ == TransactionState::ABORTING) {
            return true;
        }
        state_ = TransactionState::ABORTING;
        return true;
    }

    void SetWaitingIfNotAborting() {
        std::lock_guard<std::mutex> guard(latch_);
        if (state_ == TransactionState::ACTIVE || state_ == TransactionState::WAITING) {
            state_ = TransactionState::WAITING;
        }
    }

    void SetActiveIfNotAborting() {
        std::lock_guard<std::mutex> guard(latch_);
        if (state_ == TransactionState::ACTIVE || state_ == TransactionState::WAITING) {
            state_ = TransactionState::ACTIVE;
        }
    }

    void WaitUntilAbortCompletes() {
        std::unique_lock<std::mutex> lock(latch_);
        state_cv_.wait(lock, [this] { return state_ != TransactionState::ABORTING; });
    }

    LSN GetLastLSN() const {
        std::lock_guard<std::mutex> guard(latch_);
        return last_lsn_;
    }

    void SetLastLSN(LSN lsn) {
        std::lock_guard<std::mutex> guard(latch_);
        last_lsn_ = lsn;
    }

    std::optional<LockMode> GetTableLockMode(RelationId relation_id) const {
        std::lock_guard<std::mutex> guard(latch_);
        auto it = table_locks_.find(relation_id);
        if (it == table_locks_.end()) return std::nullopt;
        return it->second;
    }

    std::optional<LockMode> GetRecordLockMode(const RecordLockId &id) const {
        std::lock_guard<std::mutex> guard(latch_);
        auto it = record_locks_.find(id);
        if (it == record_locks_.end()) return std::nullopt;
        return it->second;
    }

    bool HoldsTableIS(RelationId relation_id) const {
        std::optional<LockMode> held = GetTableLockMode(relation_id);
        return held.has_value() &&
               (*held == LockMode::IS || *held == LockMode::IX || *held == LockMode::S ||
                *held == LockMode::SIX || *held == LockMode::X);
    }

    bool HoldsTableIX(RelationId relation_id) const {
        std::optional<LockMode> held = GetTableLockMode(relation_id);
        return held.has_value() &&
               (*held == LockMode::IX || *held == LockMode::SIX || *held == LockMode::X);
    }

    bool HoldsTableS(RelationId relation_id) const {
        std::optional<LockMode> held = GetTableLockMode(relation_id);
        return held.has_value() &&
               (*held == LockMode::S || *held == LockMode::SIX || *held == LockMode::X);
    }

    bool HoldsTableSIX(RelationId relation_id) const {
        std::optional<LockMode> held = GetTableLockMode(relation_id);
        return held.has_value() && (*held == LockMode::SIX || *held == LockMode::X);
    }

    bool HoldsTableX(RelationId relation_id) const {
        std::optional<LockMode> held = GetTableLockMode(relation_id);
        return held.has_value() && (*held == LockMode::X);
    }

    bool HoldsRecordS(const RecordLockId &id) const {
        std::optional<LockMode> held = GetRecordLockMode(id);
        return held.has_value() && (*held == LockMode::S || *held == LockMode::X);
    }

    bool HoldsRecordX(const RecordLockId &id) const {
        std::optional<LockMode> held = GetRecordLockMode(id);
        return held.has_value() && (*held == LockMode::X);
    }

    void AddTableLock(RelationId relation_id, LockMode mode) {
        std::lock_guard<std::mutex> guard(latch_);
        table_locks_[relation_id] = mode;
    }

    void AddRecordLock(const RecordLockId &id, LockMode mode) {
        std::lock_guard<std::mutex> guard(latch_);
        record_locks_[id] = mode;
    }

    std::size_t CountRecordLocksOnRelation(RelationId relation_id) const {
        std::lock_guard<std::mutex> guard(latch_);
        std::size_t count = 0;
        for (const auto &entry : record_locks_) {
            if (entry.first.relation_id == relation_id) {
                ++count;
            }
        }
        return count;
    }

    void ClearAllLocks() {
        std::lock_guard<std::mutex> guard(latch_);
        table_locks_.clear();
        record_locks_.clear();
    }

private:
    TxnId txn_id_;
    mutable std::mutex latch_;
    mutable std::condition_variable state_cv_;
    TransactionState state_{TransactionState::ACTIVE};
    LSN last_lsn_{0};

    std::unordered_map<RelationId, LockMode> table_locks_;
    std::unordered_map<RecordLockId, LockMode, RecordLockIdHash> record_locks_;
};

using TransactionPtr = std::shared_ptr<Transaction>;

}  // namespace simpledb
