#include "heap_file_iterator.h"

#include <stdexcept>

namespace simpledb {

HeapFileIterator::HeapFileIterator(const HeapFile *heap_file, bool end)
    : HeapFileIterator(heap_file, nullptr, nullptr, end) {}

HeapFileIterator::HeapFileIterator(const HeapFile *heap_file,
                                   const TransactionPtr &txn,
                                   LockManager *lock_manager,
                                   bool end)
    : heap_file_(heap_file), txn_(txn), lock_manager_(lock_manager), current_page_no_(0), current_slot_no_(0), at_end_(end) {
    if (!at_end_) AdvanceToNextValid();
}

bool HeapFileIterator::HasNext() const { return !at_end_; }

std::pair<RID, Tuple> HeapFileIterator::Next() {
    if (at_end_) throw std::runtime_error("HeapFileIterator::Next called at end");
    RID rid{current_page_no_, current_slot_no_};
    Tuple tuple;
    bool ok = txn_ != nullptr ? heap_file_->GetTuple(txn_, rid, &tuple) : heap_file_->GetTuple(rid, &tuple);
    if (!ok) throw std::runtime_error("Iterator points to invalid tuple");
    current_slot_no_++;
    AdvanceToNextValid();
    return {rid, tuple};
}

void HeapFileIterator::AdvanceToNextValid() {
    uint32_t num_pages = heap_file_->GetNumPages();
    while (current_page_no_ < num_pages) {
        PageId pid{heap_file_->GetRelationId(), current_page_no_};
        Page *page = heap_file_->GetBufferPoolManager()->FetchPage(pid);
        if (page == nullptr) {
            current_page_no_++;
            current_slot_no_ = 0;
            continue;
        }

        page->RLatch();
        SlottedPage sp(page);
        bool found = false;
        if (sp.IsInitialized() && sp.GetPageType() == PageType::HEAP) {
            while (current_slot_no_ < sp.GetSlotCount()) {
                const char *raw = nullptr;
                uint16_t raw_len = 0;
                if (sp.GetRecord(current_slot_no_, &raw, &raw_len) && !HeapTupleCodec::IsDeleted(raw, raw_len)) {
                    found = true;
                    break;
                }
                current_slot_no_++;
            }
        }

        page->RUnlatch();
        heap_file_->GetBufferPoolManager()->UnpinPage(pid, false);
        if (found) return;
        current_page_no_++;
        current_slot_no_ = 0;
    }
    at_end_ = true;
}

}  // namespace simpledb
