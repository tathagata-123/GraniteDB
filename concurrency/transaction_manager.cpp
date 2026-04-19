#include "transaction_manager.h"

#include <cstring>
#include <stdexcept>
#include <unordered_map>

#include "../recovery/wal_records.h"
#include "../storage/page_lsn_util.h"

namespace simpledb {

TransactionManager::TransactionManager(LockManager *lock_manager, LogManager *log_manager, BufferPoolManager *buffer_pool_manager)
    : lock_manager_(lock_manager), log_manager_(log_manager), buffer_pool_manager_(buffer_pool_manager) {
    if (lock_manager_ == nullptr || log_manager_ == nullptr) {
        throw std::runtime_error("TransactionManager requires valid LockManager and LogManager");
    }
    lock_manager_->SetAbortCallback([this](const TransactionPtr &txn) {
        if (txn != nullptr && txn->GetState() == TransactionState::ABORTING) {
            this->Abort(txn);
        }
    });
}

TransactionPtr TransactionManager::Begin() {
    TxnId txn_id = next_txn_id_.fetch_add(1);
    TransactionPtr txn = std::make_shared<Transaction>(txn_id);
    txn->SetState(TransactionState::ACTIVE);
    LogRecord begin; begin.type = LogRecordType::BEGIN; begin.txn_id = txn_id; begin.prev_lsn = 0;
    LSN lsn = log_manager_->AppendRecord(begin);
    txn->SetLastLSN(lsn);
    lock_manager_->RegisterTransaction(txn);
    std::lock_guard<std::mutex> guard(latch_);
    active_txns_[txn_id] = txn;
    return txn;
}

bool TransactionManager::Commit(const TransactionPtr &txn) {
    TransactionState state = txn->GetState();
    if (state == TransactionState::ABORTING) {
        txn->WaitUntilAbortCompletes();
        return false;
    }
    if (state == TransactionState::ABORTED) {
        return false;
    }
    LogRecord commit; commit.type = LogRecordType::COMMIT; commit.txn_id = txn->GetTransactionId(); commit.prev_lsn = txn->GetLastLSN();
    LSN commit_lsn = log_manager_->AppendRecord(commit);
    txn->SetLastLSN(commit_lsn);
    log_manager_->FlushUpTo(commit_lsn);
    txn->SetState(TransactionState::COMMITTED);
    lock_manager_->ReleaseAll(txn);
    lock_manager_->UnregisterTransaction(txn->GetTransactionId());
    std::lock_guard<std::mutex> guard(latch_);
    active_txns_.erase(txn->GetTransactionId());
    return true;
}

void TransactionManager::ApplyImage(PageId page_id, const std::vector<char> &image, LSN page_lsn) {
    if (buffer_pool_manager_ == nullptr) return;
    if (image.size() != PAGE_SIZE) throw std::runtime_error("Transaction rollback requires full-page images");
    Page *page = buffer_pool_manager_->FetchPage(page_id);
    if (page == nullptr) throw std::runtime_error("Failed to fetch page during abort rollback");
    page->WLatch();
    std::memcpy(page->GetData(), image.data(), PAGE_SIZE);
    SetPageLSN(page, page_lsn);
    page->WUnlatch();
    buffer_pool_manager_->UnpinPage(page_id, true);
}

void TransactionManager::RollbackTransaction(const TransactionPtr &txn) {
    if (buffer_pool_manager_ == nullptr) return;

    std::vector<LogRecord> all_records = log_manager_->ReadAllRecordsFrom(sizeof(LSN));
    std::unordered_map<LSN, LogRecord> by_lsn;
    by_lsn.reserve(all_records.size());
    for (const auto &rec : all_records) by_lsn[rec.lsn] = rec;

    LSN current = txn->GetLastLSN();
    while (current != 0) {
        auto it = by_lsn.find(current);
        if (it == by_lsn.end()) break;
        const LogRecord &rec = it->second;

        if (rec.type == LogRecordType::BEGIN) break;

        if (rec.type == LogRecordType::CLR) {
            current = rec.undo_next_lsn;
            continue;
        }

        if (IsPageUpdateRecord(rec.type) && rec.has_page) {
            LogRecord clr = MakeClrPageUndoRecord(
                txn->GetTransactionId(), txn->GetLastLSN(), rec.prev_lsn, rec.page_id, rec.before_image);
            LSN clr_lsn = log_manager_->AppendRecord(clr);
            txn->SetLastLSN(clr_lsn);
            ApplyImage(rec.page_id, rec.before_image, clr_lsn);
            current = rec.prev_lsn;
            continue;
        }

        current = rec.prev_lsn;
    }

    buffer_pool_manager_->FlushAllPages();
}

bool TransactionManager::Abort(const TransactionPtr &txn) {
    TransactionState initial_state = txn->GetState();
    if (initial_state == TransactionState::COMMITTED) return false;
    if (initial_state == TransactionState::ABORTED) return false;

    if (initial_state != TransactionState::ABORTING) {
        if (!txn->TryMarkAborting()) {
            txn->WaitUntilAbortCompletes();
            return txn->GetState() == TransactionState::ABORTED;
        }
    }

    RollbackTransaction(txn);

    LogRecord abort; abort.type = LogRecordType::ABORT; abort.txn_id = txn->GetTransactionId(); abort.prev_lsn = txn->GetLastLSN();
    LSN abort_lsn = log_manager_->AppendRecord(abort);
    txn->SetLastLSN(abort_lsn);
    log_manager_->FlushUpTo(abort_lsn);
    txn->SetState(TransactionState::ABORTED);
    lock_manager_->ReleaseAll(txn);
    lock_manager_->UnregisterTransaction(txn->GetTransactionId());
    std::lock_guard<std::mutex> guard(latch_);
    active_txns_.erase(txn->GetTransactionId());
    return true;
}

std::unordered_map<TxnId, std::pair<TransactionState, LSN>> TransactionManager::GetTransactionTableSnapshot() const {
    std::lock_guard<std::mutex> guard(latch_);
    std::unordered_map<TxnId, std::pair<TransactionState, LSN>> out;
    for (const auto &[txn_id, txn] : active_txns_) out[txn_id] = {txn->GetState(), txn->GetLastLSN()};
    return out;
}

}  // namespace simpledb
