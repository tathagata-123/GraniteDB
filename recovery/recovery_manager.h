#pragma once

#include <unordered_map>
#include <vector>

#include "../buffer/buffer_pool_manager.h"
#include "../common/types.h"
#include "log_manager.h"
#include "wal_records.h"

namespace simpledb {

class RecoveryManager {
public:
    RecoveryManager(BufferPoolManager *buffer_pool_manager, LogManager *log_manager);
    void Recover();

private:
    struct TxnTableEntry {
        TransactionState state{TransactionState::ACTIVE};
        LSN last_lsn{0};
    };

    void Analysis(const std::vector<LogRecord> &all_records);
    void Redo(const std::vector<LogRecord> &all_records);
    void Undo(const std::vector<LogRecord> &all_records);

    void ApplyImage(PageId page_id, const std::vector<char> &image, LSN page_lsn);
    bool ShouldRedoRecord(const LogRecord &rec) const;
    void RedoRecord(const LogRecord &rec);
    void UndoRecord(const LogRecord &rec, TxnId txn_id, TxnTableEntry *txn_entry);
    void FinishAbortingTxn(TxnId txn_id, TxnTableEntry *txn_entry, LSN prev_lsn);

    BufferPoolManager *buffer_pool_manager_;
    LogManager *log_manager_;
    std::unordered_map<TxnId, TxnTableEntry> txn_table_;
    std::unordered_map<PageId, LSN, PageIdHash> dirty_page_table_;
};

}  // namespace simpledb
