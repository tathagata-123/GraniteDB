#include "recovery_manager.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_map>

#include "fault_injector.h"
#include "../storage/page_lsn_util.h"

namespace simpledb {

namespace {

bool IsTxnEndState(TransactionState state) {
    return state == TransactionState::COMMITTED ||
           state == TransactionState::ABORTED;
}

bool IsLoserState(TransactionState state) {
    return state == TransactionState::ACTIVE ||
           state == TransactionState::WAITING ||
           state == TransactionState::ABORTING;
}

}  // namespace

RecoveryManager::RecoveryManager(BufferPoolManager *buffer_pool_manager,
                                 LogManager *log_manager)
    : buffer_pool_manager_(buffer_pool_manager),
      log_manager_(log_manager) {
    if (buffer_pool_manager_ == nullptr || log_manager_ == nullptr) {
        throw std::runtime_error(
            "RecoveryManager requires valid BufferPoolManager and LogManager");
    }
}

void RecoveryManager::ApplyImage(PageId page_id,
                                 const std::vector<char> &image,
                                 LSN page_lsn) {
    if (image.size() != PAGE_SIZE) {
        throw std::runtime_error("Recovery image must be exactly one page");
    }

    Page *page = buffer_pool_manager_->FetchPage(page_id);
    if (page == nullptr) {
        throw std::runtime_error("Recovery failed to fetch page");
    }

    page->WLatch();
    std::memcpy(page->GetData(), image.data(), PAGE_SIZE);
    SetPageLSN(page, page_lsn);
    page->WUnlatch();
    buffer_pool_manager_->UnpinPage(page_id, true);
}

void RecoveryManager::Analysis(const std::vector<LogRecord> &all_records) {
    txn_table_.clear();
    dirty_page_table_.clear();

    LSN checkpoint_lsn = log_manager_->GetMasterCheckpointLSN();

    for (const auto &rec : all_records) {
        if (checkpoint_lsn != 0 && rec.lsn < checkpoint_lsn) {
            continue;
        }

        switch (rec.type) {
            case LogRecordType::BEGIN: {
                auto &entry = txn_table_[rec.txn_id];
                entry.state = TransactionState::ACTIVE;
                entry.last_lsn = rec.lsn;
                break;
            }

            case LogRecordType::COMMIT: {
                auto &entry = txn_table_[rec.txn_id];
                entry.state = TransactionState::COMMITTED;
                entry.last_lsn = rec.lsn;
                break;
            }

            case LogRecordType::ABORT: {
                auto &entry = txn_table_[rec.txn_id];
                entry.state = TransactionState::ABORTED;
                entry.last_lsn = rec.lsn;
                break;
            }

            case LogRecordType::HEAP_INSERT:
            case LogRecordType::HEAP_DELETE:
            case LogRecordType::HEAP_UPDATE:
            case LogRecordType::BTREE_INSERT:
            case LogRecordType::BTREE_DELETE:
            case LogRecordType::BTREE_PAGE_SPLIT:
            case LogRecordType::BTREE_REBALANCE:
            case LogRecordType::BTREE_MERGE:
            case LogRecordType::BTREE_META_UPDATE:
            case LogRecordType::CLR: {
                auto &entry = txn_table_[rec.txn_id];
                if (!IsTxnEndState(entry.state)) {
                    entry.state = TransactionState::ACTIVE;
                }
                entry.last_lsn = std::max(entry.last_lsn, rec.lsn);

                if (rec.has_page) {
                    auto it = dirty_page_table_.find(rec.page_id);
                    if (it == dirty_page_table_.end()) {
                        dirty_page_table_[rec.page_id] = rec.lsn;
                    } else {
                        it->second = std::min(it->second, rec.lsn);
                    }
                }
                break;
            }

            case LogRecordType::END_CHECKPOINT: {
                for (const auto &tx : rec.active_txns) {
                    auto &entry = txn_table_[tx.txn_id];
                    if (!IsTxnEndState(entry.state)) {
                        entry.state = tx.state;
                    }
                    entry.last_lsn = std::max(entry.last_lsn, tx.last_lsn);
                }

                for (const auto &dp : rec.dirty_pages) {
                    auto it = dirty_page_table_.find(dp.page_id);
                    if (it == dirty_page_table_.end()) {
                        dirty_page_table_[dp.page_id] = dp.rec_lsn;
                    } else {
                        it->second = std::min(it->second, dp.rec_lsn);
                    }
                }
                break;
            }

            case LogRecordType::BEGIN_CHECKPOINT:
            case LogRecordType::INVALID:
                break;
        }
    }
}

bool RecoveryManager::ShouldRedoRecord(const LogRecord &rec) const {
    if (!IsPageUpdateRecord(rec.type) || !rec.has_page || rec.after_image.empty()) {
        return false;
    }

    auto dpt_it = dirty_page_table_.find(rec.page_id);
    if (dpt_it == dirty_page_table_.end()) {
        return false;
    }
    if (rec.lsn < dpt_it->second) {
        return false;
    }

    Page *page = buffer_pool_manager_->FetchPage(rec.page_id);
    if (page == nullptr) {
        throw std::runtime_error("Redo failed to fetch page");
    }

    page->RLatch();
    LSN page_lsn = GetPageLSN(page);
    page->RUnlatch();
    buffer_pool_manager_->UnpinPage(rec.page_id, false);

    return page_lsn < rec.lsn;
}

void RecoveryManager::RedoRecord(const LogRecord &rec) {
    FaultInjector::MaybeCrash(FaultPoint::BEFORE_REDO_APPLY);
    ApplyImage(rec.page_id, rec.after_image, rec.lsn);
    FaultInjector::MaybeCrash(FaultPoint::AFTER_REDO_APPLY);
}

void RecoveryManager::Redo(const std::vector<LogRecord> &all_records) {
    if (dirty_page_table_.empty()) {
        return;
    }

    LSN redo_start = std::numeric_limits<LSN>::max();
    for (const auto &[page_id, rec_lsn] : dirty_page_table_) {
        (void)page_id;
        redo_start = std::min(redo_start, rec_lsn);
    }

    for (const auto &rec : all_records) {
        if (rec.lsn < redo_start) {
            continue;
        }
        if (!ShouldRedoRecord(rec)) {
            continue;
        }
        RedoRecord(rec);
    }

    buffer_pool_manager_->FlushAllPages();
}

void RecoveryManager::FinishAbortingTxn(TxnId txn_id,
                                        TxnTableEntry *txn_entry,
                                        LSN prev_lsn) {
    LogRecord abort;
    abort.type = LogRecordType::ABORT;
    abort.txn_id = txn_id;
    abort.prev_lsn = prev_lsn;

    LSN abort_lsn = log_manager_->AppendRecord(abort);
    log_manager_->FlushUpTo(abort_lsn);

    txn_entry->state = TransactionState::ABORTED;
    txn_entry->last_lsn = abort_lsn;
}

void RecoveryManager::UndoRecord(const LogRecord &rec,
                                 TxnId txn_id,
                                 TxnTableEntry *txn_entry) {
    LogRecord clr = MakeClrPageUndoRecord(
        txn_id, txn_entry->last_lsn, rec.prev_lsn, rec.page_id, rec.before_image);

    LSN clr_lsn = log_manager_->AppendRecord(clr);
    txn_entry->last_lsn = clr_lsn;

    FaultInjector::MaybeCrash(FaultPoint::BEFORE_UNDO_APPLY);
    ApplyImage(rec.page_id, rec.before_image, clr_lsn);
    FaultInjector::MaybeCrash(FaultPoint::AFTER_UNDO_APPLY);
}

void RecoveryManager::Undo(const std::vector<LogRecord> &all_records) {
    std::unordered_map<LSN, LogRecord> by_lsn;
    by_lsn.reserve(all_records.size());
    for (const auto &rec : all_records) {
        by_lsn[rec.lsn] = rec;
    }

    struct HeapItem {
        LSN lsn;
        TxnId txn_id;

        bool operator<(const HeapItem &other) const {
            return lsn < other.lsn;
        }
    };

    std::priority_queue<HeapItem> pq;
    for (const auto &[txn_id, entry] : txn_table_) {
        if (IsLoserState(entry.state) && entry.last_lsn != 0) {
            pq.push(HeapItem{entry.last_lsn, txn_id});
        }
    }

    while (!pq.empty()) {
        HeapItem item = pq.top();
        pq.pop();

        auto txn_it = txn_table_.find(item.txn_id);
        if (txn_it == txn_table_.end()) {
            continue;
        }

        auto rec_it = by_lsn.find(item.lsn);
        if (rec_it == by_lsn.end()) {
            continue;
        }

        const LogRecord &rec = rec_it->second;

        if (rec.type == LogRecordType::CLR) {
            if (rec.undo_next_lsn != 0) {
                pq.push(HeapItem{rec.undo_next_lsn, item.txn_id});
            } else {
                FinishAbortingTxn(item.txn_id, &txn_it->second, rec.lsn);
            }
            continue;
        }

        if (rec.type == LogRecordType::BEGIN) {
            FinishAbortingTxn(item.txn_id, &txn_it->second, rec.lsn);
            continue;
        }

        if (IsUndoablePageRecord(rec.type) && rec.has_page) {
            UndoRecord(rec, item.txn_id, &txn_it->second);
            if (rec.prev_lsn != 0) {
                pq.push(HeapItem{rec.prev_lsn, item.txn_id});
            } else {
                FinishAbortingTxn(item.txn_id, &txn_it->second, txn_it->second.last_lsn);
            }
            continue;
        }

        if (rec.prev_lsn != 0) {
            pq.push(HeapItem{rec.prev_lsn, item.txn_id});
        } else {
            FinishAbortingTxn(item.txn_id, &txn_it->second, txn_it->second.last_lsn);
        }
    }

    buffer_pool_manager_->FlushAllPages();
}

void RecoveryManager::Recover() {
    std::vector<LogRecord> all_records =
        log_manager_->ReadAllRecordsFrom(sizeof(LSN));

    Analysis(all_records);
    Redo(all_records);
    Undo(all_records);
}

}  // namespace simpledb
