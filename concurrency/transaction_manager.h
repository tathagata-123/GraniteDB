#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "../buffer/buffer_pool_manager.h"
#include "../recovery/log_manager.h"
#include "lock_manager.h"
#include "transaction.h"

namespace simpledb {

class TransactionManager {
public:
    TransactionManager(LockManager *lock_manager, LogManager *log_manager, BufferPoolManager *buffer_pool_manager = nullptr);

    TransactionPtr Begin();
    bool Commit(const TransactionPtr &txn);
    bool Abort(const TransactionPtr &txn);

    std::unordered_map<TxnId, std::pair<TransactionState, LSN>> GetTransactionTableSnapshot() const;

private:
    void ApplyImage(PageId page_id, const std::vector<char> &image, LSN page_lsn);
    void RollbackTransaction(const TransactionPtr &txn);

    std::atomic<TxnId> next_txn_id_{1};
    LockManager *lock_manager_;
    LogManager *log_manager_;
    BufferPoolManager *buffer_pool_manager_;
    mutable std::mutex latch_;
    std::unordered_map<TxnId, TransactionPtr> active_txns_;
};

}  // namespace simpledb
