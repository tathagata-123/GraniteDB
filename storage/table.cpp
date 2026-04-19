#include "table.h"

#include "../access/heap_file.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string>

namespace simpledb {
namespace {

std::string BuildRollbackFailureMessage(const std::string &base_message,
                                       const std::vector<std::string> &rollback_errors) {
    if (rollback_errors.empty()) {
        return base_message;
    }
    std::ostringstream out;
    out << base_message << " | rollback problems:";
    for (const auto &e : rollback_errors) {
        out << " [" << e << "]";
    }
    return out.str();
}

}  // namespace

Table::Table(CatalogManager *catalog, RelationId relation_id)
    : catalog_(catalog), relation_id_(relation_id) {
    if (catalog_ == nullptr) {
        throw std::runtime_error("Table requires a non-null catalog");
    }
    const auto &rel = catalog_->GetRelation(relation_id_);
    if (rel.heap_file == nullptr) {
        throw std::runtime_error("Table requires an attached heap file");
    }
}

void Table::ValidateUnsafeNonTransactionalWriteAllowed(const char *operation_name) const {
    const RelationCatalogEntry &rel = GetRelation();
    if (rel.heap_file == nullptr || rel.heap_file->GetLockManager() == nullptr) return;
    throw std::runtime_error(std::string(operation_name) +
                             " without a transaction is disabled when locking is enabled; "
                             "use the txn-aware overload or reserve this path for bootstrap/single-threaded use");
}

RID Table::InsertTuple(const Tuple &tuple) {
    ValidateUnsafeNonTransactionalWriteAllowed("Table::InsertTuple");
    return InsertTuple(nullptr, tuple);
}

RID Table::InsertTuple(const TransactionPtr &txn, const Tuple &tuple) {
    ValidateIndexKeyCompatibility(tuple);
    AcquireIndexRelationWriteLocks(txn);
    AcquireAllIndexMutationLocks(txn, tuple);
    AcquireUniqueIndexLocks(txn, tuple);
    ValidateUniqueIndexes(tuple, std::nullopt);

    RelationCatalogEntry &rel = GetRelationMutable();
    RID rid = rel.heap_file->InsertTuple(txn, tuple);

    std::vector<const IndexCatalogEntry *> inserted_indexes;
    try {
        for (const IndexCatalogEntry *index : GetMaintenanceOrder()) {
            std::vector<Value> key = ExtractKey(tuple, *index);
            index->runtime_index->InsertEntry(txn, key, rid);
            inserted_indexes.push_back(index);
        }
        return rid;
    } catch (const std::exception &ex) {
        std::vector<std::string> rollback_errors;

        for (auto it = inserted_indexes.rbegin(); it != inserted_indexes.rend(); ++it) {
            try {
                std::vector<Value> key = ExtractKey(tuple, **it);
                if (!(*it)->runtime_index->DeleteEntry(txn, key, rid)) {
                    rollback_errors.push_back("failed to rollback inserted index entry on " + (*it)->index_name);
                }
            } catch (const std::exception &rollback_ex) {
                rollback_errors.push_back("rollback delete failed on " + (*it)->index_name + ": " + rollback_ex.what());
            }
        }

        try {
            if (!rel.heap_file->DeleteTuple(txn, rid)) {
                rollback_errors.push_back("failed to rollback inserted heap tuple");
            }
        } catch (const std::exception &rollback_ex) {
            rollback_errors.push_back(std::string("heap rollback delete failed: ") + rollback_ex.what());
        }

        throw std::runtime_error(
            BuildRollbackFailureMessage(
                "InsertTuple failed during index maintenance: " + std::string(ex.what()),
                rollback_errors));
    }
}

bool Table::GetTuple(const RID &rid, Tuple *out_tuple) const {
    return GetRelation().heap_file->GetTuple(rid, out_tuple);
}

bool Table::GetTuple(const TransactionPtr &txn, const RID &rid, Tuple *out_tuple) const {
    return GetRelation().heap_file->GetTuple(txn, rid, out_tuple);
}

bool Table::DeleteTuple(const RID &rid) {
    ValidateUnsafeNonTransactionalWriteAllowed("Table::DeleteTuple");
    return DeleteTuple(nullptr, rid);
}

bool Table::DeleteTuple(const TransactionPtr &txn, const RID &rid) {
    RelationCatalogEntry &rel = GetRelationMutable();

    Tuple old_tuple;
    if (!rel.heap_file->GetTuple(txn, rid, &old_tuple)) {
        return false;
    }

    std::vector<const IndexCatalogEntry *> deleted_indexes;
    AcquireIndexRelationWriteLocks(txn);
    AcquireAllIndexMutationLocks(txn, old_tuple);

    try {
        for (const IndexCatalogEntry *index : GetMaintenanceOrder()) {
            std::vector<Value> key = ExtractKey(old_tuple, *index);
            if (!index->runtime_index->DeleteEntry(txn, key, rid)) {
                throw std::runtime_error("index delete returned false for index " + index->index_name);
            }
            deleted_indexes.push_back(index);
        }

        if (!rel.heap_file->DeleteTuple(txn, rid)) {
            throw std::runtime_error("heap delete returned false");
        }
        return true;
    } catch (const std::exception &ex) {
        std::vector<std::string> rollback_errors;

        for (auto it = deleted_indexes.rbegin(); it != deleted_indexes.rend(); ++it) {
            try {
                std::vector<Value> key = ExtractKey(old_tuple, **it);
                (*it)->runtime_index->InsertEntry(txn, key, rid);
            } catch (const std::exception &rollback_ex) {
                rollback_errors.push_back("rollback insert failed on " + (*it)->index_name + ": " + rollback_ex.what());
            }
        }

        throw std::runtime_error(
            BuildRollbackFailureMessage(
                "DeleteTuple failed during multi-index maintenance: " + std::string(ex.what()),
                rollback_errors));
    }
}

bool Table::UpdateTuple(const RID &rid, const Tuple &new_tuple, RID *out_new_rid) {
    ValidateUnsafeNonTransactionalWriteAllowed("Table::UpdateTuple");
    return UpdateTuple(nullptr, rid, new_tuple, out_new_rid);
}

bool Table::UpdateTuple(const TransactionPtr &txn,
                        const RID &rid,
                        const Tuple &new_tuple,
                        RID *out_new_rid) {
    RelationCatalogEntry &rel = GetRelationMutable();

    Tuple old_tuple;
    if (!rel.heap_file->GetTuple(txn, rid, &old_tuple)) {
        return false;
    }

    ValidateIndexKeyCompatibility(new_tuple);

    const bool has_runtime_indexes = HasAnyRuntimeIndex();
    const bool indexed_key_changed = AnyIndexedKeyChanged(old_tuple, new_tuple);
    const bool can_update_in_place = rel.heap_file->CanUpdateTupleInPlace(txn, rid, new_tuple);

    if (!has_runtime_indexes || (!indexed_key_changed && can_update_in_place)) {
        return rel.heap_file->UpdateTuple(txn, rid, new_tuple, out_new_rid);
    }

    AcquireIndexRelationWriteLocks(txn);
    AcquireUpdateIndexMutationLocks(txn, old_tuple, new_tuple);
    AcquireUniqueIndexLocks(txn, new_tuple);
    ValidateUniqueIndexes(new_tuple, rid);

    RID new_rid = rel.heap_file->InsertTuple(txn, new_tuple);

    std::vector<const IndexCatalogEntry *> inserted_new_entries;
    std::vector<const IndexCatalogEntry *> deleted_old_entries;

    try {
        for (const IndexCatalogEntry *index : GetMaintenanceOrder()) {
            std::vector<Value> new_key = ExtractKey(new_tuple, *index);
            index->runtime_index->InsertEntry(txn, new_key, new_rid);
            inserted_new_entries.push_back(index);
        }

        for (const IndexCatalogEntry *index : GetMaintenanceOrder()) {
            std::vector<Value> old_key = ExtractKey(old_tuple, *index);
            if (!index->runtime_index->DeleteEntry(txn, old_key, rid)) {
                throw std::runtime_error("old index entry delete returned false for index " + index->index_name);
            }
            deleted_old_entries.push_back(index);
        }

        if (!rel.heap_file->DeleteTuple(txn, rid)) {
            throw std::runtime_error("old heap tuple delete returned false");
        }

        if (out_new_rid != nullptr) {
            *out_new_rid = new_rid;
        }
        return true;
    } catch (const std::exception &ex) {
        std::vector<std::string> rollback_errors;

        for (auto it = deleted_old_entries.rbegin(); it != deleted_old_entries.rend(); ++it) {
            try {
                std::vector<Value> old_key = ExtractKey(old_tuple, **it);
                (*it)->runtime_index->InsertEntry(txn, old_key, rid);
            } catch (const std::exception &rollback_ex) {
                rollback_errors.push_back("failed to restore old index entry on " + (*it)->index_name + ": " + rollback_ex.what());
            }
        }

        for (auto it = inserted_new_entries.rbegin(); it != inserted_new_entries.rend(); ++it) {
            try {
                std::vector<Value> new_key = ExtractKey(new_tuple, **it);
                if (!(*it)->runtime_index->DeleteEntry(txn, new_key, new_rid)) {
                    rollback_errors.push_back("failed to delete new index entry on " + (*it)->index_name);
                }
            } catch (const std::exception &rollback_ex) {
                rollback_errors.push_back("failed to delete new index entry on " + (*it)->index_name + ": " + rollback_ex.what());
            }
        }

        try {
            if (!rel.heap_file->DeleteTuple(txn, new_rid)) {
                rollback_errors.push_back("failed to delete newly inserted heap tuple");
            }
        } catch (const std::exception &rollback_ex) {
            rollback_errors.push_back(std::string("failed to rollback new heap tuple: ") + rollback_ex.what());
        }

        throw std::runtime_error(
            BuildRollbackFailureMessage(
                "UpdateTuple failed during multi-index maintenance: " + std::string(ex.what()),
                rollback_errors));
    }
}

const Schema &Table::GetSchema() const { return GetRelation().schema; }

HeapFile *Table::GetHeapFile() const { return GetRelation().heap_file; }

std::vector<Value> Table::ExtractKey(const Tuple &tuple, const IndexCatalogEntry &index) const {
    std::vector<Value> key_values;
    key_values.reserve(index.key_columns.size());
    for (const auto &key_col : index.key_columns) {
        key_values.push_back(tuple.GetValue(key_col.column_idx));
    }
    return key_values;
}

bool Table::KeysEqual(const std::vector<Value> &lhs, const std::vector<Value> &rhs) const {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].IsNull() || rhs[i].IsNull()) {
            if (!(lhs[i].IsNull() && rhs[i].IsNull())) return false;
            continue;
        }
        if (CompareValues(lhs[i], rhs[i]) != 0) return false;
    }
    return true;
}

bool Table::HasNullComponent(const std::vector<Value> &key_values) const {
    for (const Value &value : key_values) {
        if (value.IsNull()) return true;
    }
    return false;
}

int Table::CompareKeyValues(const std::vector<Value> &lhs, const std::vector<Value> &rhs) const {
    if (lhs.size() != rhs.size()) {
        throw std::runtime_error("Key comparison requires equal-length key vectors");
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        int cmp = CompareValues(lhs[i], rhs[i]);
        if (cmp != 0) return cmp;
    }
    return 0;
}

void Table::AcquireIndexRelationWriteLocks(const TransactionPtr &txn) const {
    if (txn == nullptr) return;
    LockManager *lock_manager = GetRelation().heap_file->GetLockManager();
    if (lock_manager == nullptr) return;

    for (const IndexCatalogEntry *index : GetMaintenanceOrder()) {
        if (!lock_manager->LockTable(txn, index->index_relation_id, LockMode::X)) {
            throw std::runtime_error("Failed to acquire index relation write lock");
        }
    }
}

void Table::AcquireIndexKeyLock(const TransactionPtr &txn,
                                RelationId index_relation_id,
                                const std::vector<Value> &key_values,
                                LockMode mode) const {
    if (txn == nullptr) return;
    LockManager *lock_manager = GetRelation().heap_file->GetLockManager();
    if (lock_manager == nullptr) return;
    if (!lock_manager->LockKey(txn, index_relation_id, key_values, mode)) {
        throw std::runtime_error("Failed to acquire index key lock");
    }
}

void Table::AcquireAllIndexMutationLocks(const TransactionPtr &txn,
                                         const Tuple &tuple) const {
    if (txn == nullptr) return;
    for (const IndexCatalogEntry *index : GetMaintenanceOrder()) {
        AcquireIndexKeyLock(txn, index->index_relation_id, ExtractKey(tuple, *index), LockMode::X);
    }
}

void Table::AcquireUpdateIndexMutationLocks(const TransactionPtr &txn,
                                            const Tuple &old_tuple,
                                            const Tuple &new_tuple) const {
    if (txn == nullptr) return;
    for (const IndexCatalogEntry *index : GetMaintenanceOrder()) {
        std::vector<Value> old_key = ExtractKey(old_tuple, *index);
        std::vector<Value> new_key = ExtractKey(new_tuple, *index);
        if (KeysEqual(old_key, new_key)) {
            AcquireIndexKeyLock(txn, index->index_relation_id, old_key, LockMode::X);
            continue;
        }

        if (CompareKeyValues(old_key, new_key) <= 0) {
            AcquireIndexKeyLock(txn, index->index_relation_id, old_key, LockMode::X);
            AcquireIndexKeyLock(txn, index->index_relation_id, new_key, LockMode::X);
        } else {
            AcquireIndexKeyLock(txn, index->index_relation_id, new_key, LockMode::X);
            AcquireIndexKeyLock(txn, index->index_relation_id, old_key, LockMode::X);
        }
    }
}

void Table::AcquireUniqueIndexLocks(const TransactionPtr &txn,
                                    const Tuple &tuple) const {
    if (txn == nullptr) return;
    const RelationCatalogEntry &rel = GetRelation();
    for (const auto &index : rel.indexes) {
        if (!index.is_unique || index.runtime_index == nullptr) continue;
        AcquireIndexKeyLock(txn, index.index_relation_id, ExtractKey(tuple, index), LockMode::X);
    }
}

std::vector<const IndexCatalogEntry *> Table::GetMaintenanceOrder() const {
    const RelationCatalogEntry &rel = GetRelation();
    std::vector<const IndexCatalogEntry *> ordered;
    for (const auto &index : rel.indexes) {
        if (index.runtime_index == nullptr) {
            throw std::runtime_error("Registered index has no attached runtime object: " + index.index_name);
        }
        ordered.push_back(&index);
    }

    std::sort(ordered.begin(), ordered.end(), [](const IndexCatalogEntry *a, const IndexCatalogEntry *b) {
        if (a->index_relation_id != b->index_relation_id) {
            return a->index_relation_id < b->index_relation_id;
        }
        return a->index_name < b->index_name;
    });
    return ordered;
}

bool Table::HasAnyRuntimeIndex() const {
    const RelationCatalogEntry &rel = GetRelation();
    for (const auto &index : rel.indexes) {
        if (index.runtime_index != nullptr) return true;
    }
    return false;
}

bool Table::AnyIndexedKeyChanged(const Tuple &old_tuple, const Tuple &new_tuple) const {
    const RelationCatalogEntry &rel = GetRelation();
    for (const auto &index : rel.indexes) {
        if (index.runtime_index == nullptr) continue;
        if (!KeysEqual(ExtractKey(old_tuple, index), ExtractKey(new_tuple, index))) {
            return true;
        }
    }
    return false;
}

void Table::ValidateIndexKeyCompatibility(const Tuple &tuple) const {
    const RelationCatalogEntry &rel = GetRelation();
    for (const auto &index : rel.indexes) {
        if (index.runtime_index == nullptr) {
            throw std::runtime_error("Registered index has no attached runtime object: " + index.index_name);
        }
        std::vector<Value> key = ExtractKey(tuple, index);
        if (HasNullComponent(key)) {
            throw std::runtime_error("NULL index keys are not supported by this storage/index layer: " + index.index_name);
        }
    }
}

void Table::ValidateUniqueIndexes(const Tuple &tuple,
                                  const std::optional<RID> &rid_to_ignore) const {
    const RelationCatalogEntry &rel = GetRelation();
    for (const auto &index : rel.indexes) {
        if (!index.is_unique) continue;
        if (index.runtime_index == nullptr) {
            throw std::runtime_error("Unique index has no attached runtime object: " + index.index_name);
        }
        std::vector<Value> key = ExtractKey(tuple, index);
        std::vector<RID> hits = index.runtime_index->SearchExact(key);
        for (const RID &hit : hits) {
            if (!rid_to_ignore.has_value() || !(hit == *rid_to_ignore)) {
                throw std::runtime_error("Unique index violation on index: " + index.index_name);
            }
        }
    }
}

const RelationCatalogEntry &Table::GetRelation() const {
    return catalog_->GetRelation(relation_id_);
}

RelationCatalogEntry &Table::GetRelationMutable() const {
    return catalog_->GetRelationMutable(relation_id_);
}

}  // namespace simpledb
