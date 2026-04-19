#include "checkpoint_manager.h"

#include <stdexcept>

namespace simpledb {

void CheckpointManager::CreateCheckpoint() {
    if (log_manager_ == nullptr || transaction_manager_ == nullptr || buffer_pool_manager_ == nullptr) {
        throw std::runtime_error("CheckpointManager requires valid LogManager, TransactionManager, and BufferPoolManager");
    }

    LogRecord begin;
    begin.type = LogRecordType::BEGIN_CHECKPOINT;
    begin.txn_id = 0;
    begin.prev_lsn = 0;

    const LSN begin_lsn = log_manager_->AppendRecord(begin);

    const auto txn_table_snapshot = transaction_manager_->GetTransactionTableSnapshot();
    const auto dirty_page_table_snapshot = buffer_pool_manager_->GetDirtyPageTableSnapshot();

    LogRecord end;
    end.type = LogRecordType::END_CHECKPOINT;
    end.txn_id = 0;
    end.prev_lsn = begin_lsn;

    for (const auto &[txn_id, info] : txn_table_snapshot) {
        CheckpointTxnEntry entry;
        entry.txn_id = txn_id;
        entry.state = info.first;
        entry.last_lsn = info.second;
        end.active_txns.push_back(entry);
    }

    for (const auto &[page_id, rec_lsn] : dirty_page_table_snapshot) {
        DirtyPageEntry entry;
        entry.page_id = page_id;
        entry.rec_lsn = rec_lsn;
        end.dirty_pages.push_back(entry);
    }

    const LSN end_lsn = log_manager_->AppendRecord(end);
    log_manager_->FlushUpTo(end_lsn);
    log_manager_->SetMasterCheckpointLSN(begin_lsn);
}

}  // namespace simpledb
