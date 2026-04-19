#include "heap_file.h"

#include <optional>
#include <stdexcept>
#include <vector>

#include "../recovery/wal_records.h"
#include "../storage/page_lsn_util.h"

namespace simpledb {

HeapFile::HeapFile(BufferPoolManager *buffer_pool_manager,
                   RelationId relation_id,
                   Schema schema,
                   FreeSpaceMap *free_space_map,
                   LogManager *log_manager,
                   LockManager *lock_manager)
    : buffer_pool_manager_(buffer_pool_manager),
      relation_id_(relation_id),
      schema_(std::move(schema)),
      free_space_map_(free_space_map),
      log_manager_(log_manager),
      lock_manager_(lock_manager) {
    if (free_space_map_ == nullptr) {
        throw std::runtime_error("HeapFile requires a valid FreeSpaceMap");
    }
}

const Schema &HeapFile::GetSchema() const { return schema_; }
RelationId HeapFile::GetRelationId() const { return relation_id_; }
uint32_t HeapFile::GetNumPages() const { return buffer_pool_manager_->GetDiskManager()->GetNumPages(relation_id_); }
BufferPoolManager *HeapFile::GetBufferPoolManager() const { return buffer_pool_manager_; }
FreeSpaceMap *HeapFile::GetFreeSpaceMap() const { return free_space_map_; }
LockManager *HeapFile::GetLockManager() const { return lock_manager_; }

void HeapFile::AcquireTableLockIfNeeded(const TransactionPtr &txn, LockMode mode) const {
    if (txn != nullptr && lock_manager_ != nullptr) {
        if (!lock_manager_->LockTable(txn, relation_id_, mode)) {
            throw std::runtime_error("Failed to acquire heap table lock");
        }
    }
}

void HeapFile::AcquireRecordLockIfNeeded(const TransactionPtr &txn, const RID &rid, LockMode mode) const {
    if (txn != nullptr && lock_manager_ != nullptr) {
        if (!lock_manager_->LockRecord(txn, RecordLockId{relation_id_, rid}, mode)) {
            throw std::runtime_error("Failed to acquire heap record lock");
        }
    }
}

void HeapFile::ValidateUnsafeNonTransactionalWriteAllowed(const char *operation_name) const {
    if (lock_manager_ == nullptr) return;
    throw std::runtime_error(std::string(operation_name) +
                             " without a transaction is disabled when locking is enabled; "
                             "use the txn-aware overload or reserve this path for bootstrap/single-threaded use");
}

RID HeapFile::InsertTuple(const Tuple &tuple) {
    ValidateUnsafeNonTransactionalWriteAllowed("HeapFile::InsertTuple");
    return InsertTupleImpl(nullptr, tuple, false);
}
RID HeapFile::InsertTuple(const TransactionPtr &txn, const Tuple &tuple) { return InsertTupleImpl(txn, tuple, true); }

RID HeapFile::InsertTupleImpl(const TransactionPtr &txn, const Tuple &tuple, bool do_log) {
    AcquireTableLockIfNeeded(txn, LockMode::X);

    std::vector<char> bytes = HeapTupleCodec::Encode(tuple, schema_);
    const uint16_t max_record_on_empty_page =
        static_cast<uint16_t>(PAGE_SIZE - sizeof(SlottedPageHeader) - sizeof(SlotEntry));
    if (bytes.size() > max_record_on_empty_page) {
        throw std::runtime_error("Tuple too large for a heap page");
    }

    uint16_t needed_bytes = static_cast<uint16_t>(bytes.size());
    std::vector<PageNo> candidates = free_space_map_->GetCandidatePages(needed_bytes);

    auto try_insert_into_page = [&](PageNo page_no) -> std::optional<RID> {
        PageId pid{relation_id_, page_no};
        Page *page = buffer_pool_manager_->FetchPage(pid);
        if (page == nullptr) return std::nullopt;

        page->WLatch();
        std::vector<char> before_image(page->GetData(), page->GetData() + PAGE_SIZE);

        SlottedPage sp(page);
        if (!sp.IsInitialized()) sp.Initialize(PageType::HEAP);

        SlotNo slot_no = 0;
        bool inserted = sp.InsertRecord(bytes.data(), needed_bytes, &slot_no);
        if (!inserted) {
            page->WUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            return std::nullopt;
        }

        RID rid{page_no, slot_no};
        AcquireRecordLockIfNeeded(txn, rid, LockMode::X);

        std::vector<char> after_image(page->GetData(), page->GetData() + PAGE_SIZE);
        if (do_log && txn != nullptr && log_manager_ != nullptr) {
            LogRecord rec;
            rec.type = LogRecordType::HEAP_INSERT;
            rec.txn_id = txn->GetTransactionId();
            rec.prev_lsn = txn->GetLastLSN();
            rec.has_page = true;
            rec.page_id = pid;
            rec.before_image = before_image;
            rec.after_image = after_image;
            LSN lsn = log_manager_->AppendRecord(rec);
            txn->SetLastLSN(lsn);
            SetPageLSN(page, lsn);
        }

        uint16_t free_after = sp.GetMaxInsertableBytesAfterCompaction();
        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(pid, true);
        free_space_map_->UpdatePageFreeSpace(page_no, free_after);
        return rid;
    };

    for (PageNo page_no : candidates) {
        if (page_no >= GetNumPages()) continue;
        auto rid = try_insert_into_page(page_no);
        if (rid.has_value()) return *rid;
    }

    PageId new_pid{};
    Page *new_page = buffer_pool_manager_->NewPage(relation_id_, &new_pid);
    if (new_page == nullptr) throw std::runtime_error("Buffer pool full: cannot allocate a new heap page");

    new_page->WLatch();
    std::vector<char> before_image(new_page->GetData(), new_page->GetData() + PAGE_SIZE);
    SlottedPage sp(new_page);
    sp.Initialize(PageType::HEAP);

    SlotNo slot_no = 0;
    bool ok = sp.InsertRecord(bytes.data(), needed_bytes, &slot_no);
    if (!ok) {
        new_page->WUnlatch();
        buffer_pool_manager_->UnpinPage(new_pid, false);
        throw std::runtime_error("Failed to insert into newly created heap page");
    }

    RID rid{new_pid.page_no, slot_no};
    AcquireRecordLockIfNeeded(txn, rid, LockMode::X);

    std::vector<char> after_image(new_page->GetData(), new_page->GetData() + PAGE_SIZE);
    if (do_log && txn != nullptr && log_manager_ != nullptr) {
        LogRecord rec;
        rec.type = LogRecordType::HEAP_INSERT;
        rec.txn_id = txn->GetTransactionId();
        rec.prev_lsn = txn->GetLastLSN();
        rec.has_page = true;
        rec.page_id = new_pid;
        rec.before_image = before_image;
        rec.after_image = after_image;
        LSN lsn = log_manager_->AppendRecord(rec);
        txn->SetLastLSN(lsn);
        SetPageLSN(new_page, lsn);
    }

    uint16_t free_after = sp.GetMaxInsertableBytesAfterCompaction();
    new_page->WUnlatch();
    buffer_pool_manager_->UnpinPage(new_pid, true);
    free_space_map_->UpdatePageFreeSpace(new_pid.page_no, free_after);
    return rid;
}

bool HeapFile::GetTuple(const RID &rid, Tuple *out_tuple) const { return GetTuple(nullptr, rid, out_tuple); }

bool HeapFile::GetTuple(const TransactionPtr &txn, const RID &rid, Tuple *out_tuple) const {
    if (rid.page_no >= GetNumPages()) return false;
    AcquireTableLockIfNeeded(txn, LockMode::IS);
    AcquireRecordLockIfNeeded(txn, rid, LockMode::S);

    PageId pid{relation_id_, rid.page_no};
    Page *page = buffer_pool_manager_->FetchPage(pid);
    if (page == nullptr) return false;

    page->RLatch();
    SlottedPage sp(page);
    bool ok = false;
    if (sp.IsInitialized() && sp.GetPageType() == PageType::HEAP) {
        const char *raw = nullptr;
        uint16_t raw_len = 0;
        if (sp.GetRecord(rid.slot_no, &raw, &raw_len) && !HeapTupleCodec::IsDeleted(raw, raw_len)) {
            if (out_tuple != nullptr) *out_tuple = HeapTupleCodec::Decode(schema_, raw, raw_len);
            ok = true;
        }
    }
    page->RUnlatch();
    buffer_pool_manager_->UnpinPage(pid, false);
    return ok;
}

bool HeapFile::DeleteTuple(const RID &rid) {
    ValidateUnsafeNonTransactionalWriteAllowed("HeapFile::DeleteTuple");
    return DeleteTupleImpl(nullptr, rid, false);
}
bool HeapFile::DeleteTuple(const TransactionPtr &txn, const RID &rid) { return DeleteTupleImpl(txn, rid, true); }

bool HeapFile::DeleteTupleImpl(const TransactionPtr &txn, const RID &rid, bool do_log) {
    if (rid.page_no >= GetNumPages()) return false;
    AcquireTableLockIfNeeded(txn, LockMode::X);
    AcquireRecordLockIfNeeded(txn, rid, LockMode::X);

    PageId pid{relation_id_, rid.page_no};
    Page *page = buffer_pool_manager_->FetchPage(pid);
    if (page == nullptr) return false;

    page->WLatch();
    std::vector<char> before_image(page->GetData(), page->GetData() + PAGE_SIZE);
    SlottedPage sp(page);
    bool ok = false;
    if (sp.IsInitialized() && sp.GetPageType() == PageType::HEAP) {
        char *raw = nullptr;
        uint16_t raw_len = 0;
        if (sp.GetMutableRecord(rid.slot_no, &raw, &raw_len) && !HeapTupleCodec::IsDeleted(raw, raw_len)) {
            HeapTupleCodec::MarkDeleted(raw, raw_len);
            ok = sp.DeleteRecord(rid.slot_no);
        }
    }

    if (!ok) {
        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        return false;
    }

    std::vector<char> after_image(page->GetData(), page->GetData() + PAGE_SIZE);
    if (do_log && txn != nullptr && log_manager_ != nullptr) {
        LogRecord rec;
        rec.type = LogRecordType::HEAP_DELETE;
        rec.txn_id = txn->GetTransactionId();
        rec.prev_lsn = txn->GetLastLSN();
        rec.has_page = true;
        rec.page_id = pid;
        rec.before_image = before_image;
        rec.after_image = after_image;
        LSN lsn = log_manager_->AppendRecord(rec);
        txn->SetLastLSN(lsn);
        SetPageLSN(page, lsn);
    }

    uint16_t free_after = sp.GetMaxInsertableBytesAfterCompaction();
    page->WUnlatch();
    buffer_pool_manager_->UnpinPage(pid, true);
    free_space_map_->UpdatePageFreeSpace(rid.page_no, free_after);
    return true;
}


bool HeapFile::CanUpdateTupleInPlace(const RID &rid, const Tuple &new_tuple) const {
    return CanUpdateTupleInPlace(nullptr, rid, new_tuple);
}

bool HeapFile::CanUpdateTupleInPlace(const TransactionPtr &txn,
                                     const RID &rid,
                                     const Tuple &new_tuple) const {
    if (rid.page_no >= GetNumPages()) return false;

    AcquireTableLockIfNeeded(txn, LockMode::IS);
    AcquireRecordLockIfNeeded(txn, rid, LockMode::S);

    std::vector<char> new_bytes = HeapTupleCodec::Encode(new_tuple, schema_);

    PageId pid{relation_id_, rid.page_no};
    Page *page = buffer_pool_manager_->FetchPage(pid);
    if (page == nullptr) return false;

    page->RLatch();
    SlottedPage sp(page);
    bool can_update_in_place = false;

    if (sp.IsInitialized() && sp.GetPageType() == PageType::HEAP) {
        const char *raw = nullptr;
        uint16_t raw_len = 0;
        if (sp.GetRecord(rid.slot_no, &raw, &raw_len) && !HeapTupleCodec::IsDeleted(raw, raw_len)) {
            can_update_in_place = (new_bytes.size() <= raw_len);
        }
    }

    page->RUnlatch();
    buffer_pool_manager_->UnpinPage(pid, false);
    return can_update_in_place;
}
bool HeapFile::UpdateTuple(const RID &old_rid, const Tuple &new_tuple, RID *out_new_rid) {
    ValidateUnsafeNonTransactionalWriteAllowed("HeapFile::UpdateTuple");
    return UpdateTupleImpl(nullptr, old_rid, new_tuple, out_new_rid, false);
}

bool HeapFile::UpdateTuple(const TransactionPtr &txn,
                           const RID &old_rid,
                           const Tuple &new_tuple,
                           RID *out_new_rid) {
    return UpdateTupleImpl(txn, old_rid, new_tuple, out_new_rid, true);
}

bool HeapFile::UpdateTupleImpl(const TransactionPtr &txn,
                               const RID &old_rid,
                               const Tuple &new_tuple,
                               RID *out_new_rid,
                               bool do_log) {
    if (old_rid.page_no >= GetNumPages()) return false;
    AcquireTableLockIfNeeded(txn, LockMode::X);
    AcquireRecordLockIfNeeded(txn, old_rid, LockMode::X);

    std::vector<char> new_bytes = HeapTupleCodec::Encode(new_tuple, schema_);
    PageId pid{relation_id_, old_rid.page_no};
    Page *page = buffer_pool_manager_->FetchPage(pid);
    if (page == nullptr) return false;

    page->WLatch();
    std::vector<char> before_image(page->GetData(), page->GetData() + PAGE_SIZE);
    SlottedPage sp(page);
    bool found = false;
    bool done_in_place = false;

    if (sp.IsInitialized() && sp.GetPageType() == PageType::HEAP) {
        char *raw = nullptr;
        uint16_t raw_len = 0;
        if (sp.GetMutableRecord(old_rid.slot_no, &raw, &raw_len) && !HeapTupleCodec::IsDeleted(raw, raw_len)) {
            found = true;
            if (new_bytes.size() <= raw_len) {
                done_in_place = sp.OverwriteRecord(old_rid.slot_no, new_bytes.data(), static_cast<uint16_t>(new_bytes.size()));
            }
        }
    }

    if (!found) {
        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        return false;
    }

    if (done_in_place) {
        std::vector<char> after_image(page->GetData(), page->GetData() + PAGE_SIZE);
        if (do_log && txn != nullptr && log_manager_ != nullptr) {
            LogRecord rec;
            rec.type = LogRecordType::HEAP_UPDATE;
            rec.txn_id = txn->GetTransactionId();
            rec.prev_lsn = txn->GetLastLSN();
            rec.has_page = true;
            rec.page_id = pid;
            rec.before_image = before_image;
            rec.after_image = after_image;
            LSN lsn = log_manager_->AppendRecord(rec);
            txn->SetLastLSN(lsn);
            SetPageLSN(page, lsn);
        }

        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(pid, true);
        if (out_new_rid != nullptr) *out_new_rid = old_rid;
        return true;
    }

    page->WUnlatch();
    buffer_pool_manager_->UnpinPage(pid, false);

    RID new_rid = InsertTupleImpl(txn, new_tuple, do_log);
    if (txn != nullptr && lock_manager_ != nullptr) {
        AcquireRecordLockIfNeeded(txn, new_rid, LockMode::X);
    }
    if (!DeleteTupleImpl(txn, old_rid, do_log)) return false;
    if (out_new_rid != nullptr) *out_new_rid = new_rid;
    return true;
}

}  // namespace simpledb
