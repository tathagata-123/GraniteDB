#pragma once

#include <utility>

#include "../concurrency/lock_manager.h"
#include "../concurrency/transaction.h"
#include "heap_file.h"

namespace simpledb {

class HeapFileIterator {
public:
    explicit HeapFileIterator(const HeapFile *heap_file, bool end = false);
    HeapFileIterator(const HeapFile *heap_file,
                     const TransactionPtr &txn,
                     LockManager *lock_manager,
                     bool end = false);

    bool HasNext() const;
    std::pair<RID, Tuple> Next();

private:
    void AdvanceToNextValid();

    const HeapFile *heap_file_;
    TransactionPtr txn_;
    LockManager *lock_manager_;
    PageNo current_page_no_;
    SlotNo current_slot_no_;
    bool at_end_;
};

}  // namespace simpledb
