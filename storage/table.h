#pragma once

#include <optional>
#include <vector>

#include "../catalog/catalog_manager.h"
#include "../common/tuple.h"
#include "../concurrency/transaction.h"
#include "../execution/expressions.h"

namespace simpledb {

class Table {
public:
    Table(CatalogManager *catalog, RelationId relation_id);

    RID InsertTuple(const Tuple &tuple);
    RID InsertTuple(const TransactionPtr &txn, const Tuple &tuple);

    bool GetTuple(const RID &rid, Tuple *out_tuple) const;
    bool GetTuple(const TransactionPtr &txn, const RID &rid, Tuple *out_tuple) const;

    bool DeleteTuple(const RID &rid);
    bool DeleteTuple(const TransactionPtr &txn, const RID &rid);

    bool UpdateTuple(const RID &rid, const Tuple &new_tuple, RID *out_new_rid);
    bool UpdateTuple(const TransactionPtr &txn,
                     const RID &rid,
                     const Tuple &new_tuple,
                     RID *out_new_rid);

    const Schema &GetSchema() const;
    RelationId GetRelationId() const { return relation_id_; }
    HeapFile *GetHeapFile() const;

private:
    std::vector<Value> ExtractKey(const Tuple &tuple, const IndexCatalogEntry &index) const;
    bool KeysEqual(const std::vector<Value> &lhs, const std::vector<Value> &rhs) const;
    int CompareKeyValues(const std::vector<Value> &lhs, const std::vector<Value> &rhs) const;
    bool HasNullComponent(const std::vector<Value> &key_values) const;

    void AcquireIndexRelationWriteLocks(const TransactionPtr &txn) const;
    void AcquireIndexKeyLock(const TransactionPtr &txn,
                             RelationId index_relation_id,
                             const std::vector<Value> &key_values,
                             LockMode mode) const;
    void AcquireAllIndexMutationLocks(const TransactionPtr &txn,
                                      const Tuple &tuple) const;
    void AcquireUpdateIndexMutationLocks(const TransactionPtr &txn,
                                         const Tuple &old_tuple,
                                         const Tuple &new_tuple) const;
    void AcquireUniqueIndexLocks(const TransactionPtr &txn,
                                 const Tuple &tuple) const;

    std::vector<const IndexCatalogEntry *> GetMaintenanceOrder() const;
    bool HasAnyRuntimeIndex() const;
    bool AnyIndexedKeyChanged(const Tuple &old_tuple, const Tuple &new_tuple) const;

    void ValidateIndexKeyCompatibility(const Tuple &tuple) const;
    void ValidateUniqueIndexes(const Tuple &tuple,
                               const std::optional<RID> &rid_to_ignore) const;

    const RelationCatalogEntry &GetRelation() const;
    RelationCatalogEntry &GetRelationMutable() const;
    void ValidateUnsafeNonTransactionalWriteAllowed(const char *operation_name) const;

private:
    CatalogManager *catalog_;
    RelationId relation_id_;
};

}  // namespace simpledb
