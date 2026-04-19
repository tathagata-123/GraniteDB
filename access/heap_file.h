#pragma once

#include "../buffer/buffer_pool_manager.h"
#include "../common/schema.h"
#include "../common/tuple.h"
#include "../concurrency/lock_manager.h"
#include "../concurrency/transaction.h"
#include "../recovery/log_manager.h"
#include "../storage/free_space_map.h"
#include "../storage/heap_tuple_codec.h"
#include "../storage/slotted_page.h"

namespace simpledb {

class HeapFile {
public:
    HeapFile(BufferPoolManager *buffer_pool_manager,
             RelationId relation_id,
             Schema schema,
             FreeSpaceMap *free_space_map,
             LogManager *log_manager = nullptr,
             LockManager *lock_manager = nullptr);

    RID InsertTuple(const Tuple &tuple);
    RID InsertTuple(const TransactionPtr &txn, const Tuple &tuple);

    bool GetTuple(const RID &rid, Tuple *out_tuple) const;
    bool GetTuple(const TransactionPtr &txn, const RID &rid, Tuple *out_tuple) const;

    bool DeleteTuple(const RID &rid);
    bool DeleteTuple(const TransactionPtr &txn, const RID &rid);

    bool UpdateTuple(const RID &old_rid, const Tuple &new_tuple, RID *out_new_rid);
    bool UpdateTuple(const TransactionPtr &txn,
                     const RID &old_rid,
                     const Tuple &new_tuple,
                     RID *out_new_rid);

    bool CanUpdateTupleInPlace(const RID &rid, const Tuple &new_tuple) const;
    bool CanUpdateTupleInPlace(const TransactionPtr &txn,
                               const RID &rid,
                               const Tuple &new_tuple) const;

    const Schema &GetSchema() const;
    RelationId GetRelationId() const;
    uint32_t GetNumPages() const;

    BufferPoolManager *GetBufferPoolManager() const;
    FreeSpaceMap *GetFreeSpaceMap() const;
    LockManager *GetLockManager() const;

private:
    void AcquireTableLockIfNeeded(const TransactionPtr &txn, LockMode mode) const;
    void AcquireRecordLockIfNeeded(const TransactionPtr &txn, const RID &rid, LockMode mode) const;
    void ValidateUnsafeNonTransactionalWriteAllowed(const char *operation_name) const;

    RID InsertTupleImpl(const TransactionPtr &txn, const Tuple &tuple, bool do_log);
    bool DeleteTupleImpl(const TransactionPtr &txn, const RID &rid, bool do_log);
    bool UpdateTupleImpl(const TransactionPtr &txn,
                         const RID &old_rid,
                         const Tuple &new_tuple,
                         RID *out_new_rid,
                         bool do_log);

private:
    BufferPoolManager *buffer_pool_manager_;
    RelationId relation_id_;
    Schema schema_;
    FreeSpaceMap *free_space_map_;
    LogManager *log_manager_;
    LockManager *lock_manager_;
};

}  // namespace simpledb
