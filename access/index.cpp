#include "index.h"

#include <stdexcept>

#include "btree.h"

namespace simpledb {

const Value &BTreeIndexAdapter::ExtractSingleKey(const std::vector<Value> &key_values) const {
    if (key_values.size() != 1) {
        throw std::runtime_error("Current B+ tree adapter only supports single-column keys");
    }
    return key_values.front();
}

void BTreeIndexAdapter::InsertEntry(const std::vector<Value> &key_values, const RID &rid) {
    index_->Insert(ExtractSingleKey(key_values), rid);
}

void BTreeIndexAdapter::InsertEntry(const TransactionPtr &txn,
                                    const std::vector<Value> &key_values,
                                    const RID &rid) {
    index_->Insert(txn, ExtractSingleKey(key_values), rid);
}

bool BTreeIndexAdapter::DeleteEntry(const std::vector<Value> &key_values, const RID &rid) {
    return index_->Delete(ExtractSingleKey(key_values), rid);
}

bool BTreeIndexAdapter::DeleteEntry(const TransactionPtr &txn,
                                    const std::vector<Value> &key_values,
                                    const RID &rid) {
    return index_->Delete(txn, ExtractSingleKey(key_values), rid);
}

std::vector<RID> BTreeIndexAdapter::SearchExact(const std::vector<Value> &key_values) const {
    return index_->Search(ExtractSingleKey(key_values));
}

RelationId BTreeIndexAdapter::GetIndexRelationId() const { return index_->GetIndexRelationId(); }
PageNo BTreeIndexAdapter::GetRootPageNo() const { return index_->GetRootPageNo(); }

}  // namespace simpledb
