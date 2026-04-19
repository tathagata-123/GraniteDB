#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../common/types.h"
#include "../common/value.h"
#include "../concurrency/transaction.h"

namespace simpledb {

class BTreeIndex;

enum class IndexKind : uint8_t {
    BTREE = 0
};

enum class NullPolicy : uint8_t {
    NOT_SUPPORTED = 0,
    DISTINCT_NULLS,
    NULLS_LOW,
    NULLS_HIGH
};

struct IndexKeyColumnDefinition {
    std::size_t column_idx{0};
    TypeId type{TypeId::INVALID};
    uint32_t max_varchar_len{0};
    bool nullable{true};
};

struct IndexDefinition {
    std::string index_name;
    RelationId base_relation_id{0};
    RelationId index_relation_id{0};
    std::vector<IndexKeyColumnDefinition> key_columns;
    bool is_unique{false};
    NullPolicy null_policy{NullPolicy::NOT_SUPPORTED};
    IndexKind kind{IndexKind::BTREE};
    PageNo root_page_no{0};

    bool IsComposite() const { return key_columns.size() > 1; }
    bool IsSingleColumn() const { return key_columns.size() == 1; }
};

class AbstractIndex {
public:
    virtual ~AbstractIndex() = default;

    virtual void InsertEntry(const std::vector<Value> &key_values,
                             const RID &rid) = 0;
    virtual void InsertEntry(const TransactionPtr &txn,
                             const std::vector<Value> &key_values,
                             const RID &rid) = 0;

    virtual bool DeleteEntry(const std::vector<Value> &key_values,
                             const RID &rid) = 0;
    virtual bool DeleteEntry(const TransactionPtr &txn,
                             const std::vector<Value> &key_values,
                             const RID &rid) = 0;

    virtual std::vector<RID> SearchExact(const std::vector<Value> &key_values) const = 0;

    virtual bool SupportsDelete() const { return false; }
    virtual bool SupportsCompositeKeys() const = 0;
    virtual bool SupportsOrderedScan() const { return false; }

    virtual RelationId GetIndexRelationId() const = 0;
    virtual PageNo GetRootPageNo() const = 0;
    virtual IndexKind GetIndexKind() const = 0;
    virtual BTreeIndex *AsBTreeIndex() { return nullptr; }
    virtual const BTreeIndex *AsBTreeIndex() const { return nullptr; }
};

class BTreeIndexAdapter : public AbstractIndex {
public:
    explicit BTreeIndexAdapter(BTreeIndex *index) : index_(index) {
        if (index_ == nullptr) {
            throw std::runtime_error("BTreeIndexAdapter requires a non-null BTreeIndex");
        }
    }

    void InsertEntry(const std::vector<Value> &key_values, const RID &rid) override;
    void InsertEntry(const TransactionPtr &txn,
                     const std::vector<Value> &key_values,
                     const RID &rid) override;

    bool DeleteEntry(const std::vector<Value> &key_values, const RID &rid) override;
    bool DeleteEntry(const TransactionPtr &txn,
                     const std::vector<Value> &key_values,
                     const RID &rid) override;

    std::vector<RID> SearchExact(const std::vector<Value> &key_values) const override;

    bool SupportsDelete() const override { return true; }
    bool SupportsCompositeKeys() const override { return false; }
    bool SupportsOrderedScan() const override { return true; }

    RelationId GetIndexRelationId() const override;
    PageNo GetRootPageNo() const override;
    IndexKind GetIndexKind() const override { return IndexKind::BTREE; }
    BTreeIndex *AsBTreeIndex() override { return index_; }
    const BTreeIndex *AsBTreeIndex() const override { return index_; }

private:
    const Value &ExtractSingleKey(const std::vector<Value> &key_values) const;

private:
    BTreeIndex *index_;
};

}  // namespace simpledb
