#pragma once
#include <optional>

#include <vector>

#include "index.h"
#include "generic_btree_page.h"
#include "composite_key_codec.h"
#include "../buffer/buffer_pool_manager.h"
#include "../recovery/log_manager.h"

namespace simpledb {

class GenericBTreeIndex : public AbstractIndex {
public:
    struct PrefixScanSpec {
        std::vector<Value> equality_prefix;
        std::optional<Value> lower_bound;
        std::optional<Value> upper_bound;
        bool lower_inclusive{true};
        bool upper_inclusive{true};
    };

    GenericBTreeIndex(BufferPoolManager *buffer_pool_manager,
                      IndexDefinition definition,
                      LogManager *log_manager = nullptr);

    void InsertEntry(const std::vector<Value> &key_values, const RID &rid) override;
    void InsertEntry(const TransactionPtr &txn,
                     const std::vector<Value> &key_values,
                     const RID &rid) override;

    bool DeleteEntry(const std::vector<Value> &key_values, const RID &rid) override;
    bool DeleteEntry(const TransactionPtr &txn,
                     const std::vector<Value> &key_values,
                     const RID &rid) override;

    struct KeyRidEntry {
        std::vector<Value> key_values;
        RID rid;
    };

    std::vector<RID> SearchExact(const std::vector<Value> &key_values) const override;
    std::vector<RID> ScanPrefixRange(const PrefixScanSpec &spec) const;
    std::vector<RID> FullScanRids() const;
    std::vector<KeyRidEntry> SearchExactEntries(const std::vector<Value> &key_values) const;
    std::vector<KeyRidEntry> ScanPrefixRangeEntries(const PrefixScanSpec &spec) const;
    std::vector<KeyRidEntry> FullScanEntries() const;

    bool SupportsDelete() const override { return true; }
    bool SupportsCompositeKeys() const override { return true; }
    bool SupportsOrderedScan() const override { return true; }

    RelationId GetIndexRelationId() const override { return definition_.index_relation_id; }
    BufferPoolManager *GetBufferPoolManager() const { return buffer_pool_manager_; }
    uint32_t GetMaxKeyBytes() const { return max_key_bytes_; }
    PageNo GetRootPageNo() const override;
    IndexKind GetIndexKind() const override { return IndexKind::BTREE; }
    const IndexDefinition &GetDefinition() const { return definition_; }

private:
    struct SplitResult {
        bool did_split{false};
        std::vector<char> separator_key;
        PageNo right_page_no{INVALID_PAGE_NO};
    };

    struct DeleteResult {
        bool deleted{false};
        bool underflow{false};
        bool subtree_first_key_changed{false};
        std::vector<char> new_subtree_first_key;
    };

    void InitializeIfNeeded();
    BTreeMetaPageData ReadMetaPage() const;
    void WriteMetaPage(const BTreeMetaPageData &meta, const TransactionPtr &txn = nullptr);
    void LogPageChange(const TransactionPtr &txn,
                       LogRecordType type,
                       Page *page,
                       PageId page_id,
                       const std::vector<char> &before_image,
                       const std::vector<char> &after_image) const;

    std::vector<char> EncodeKey(const std::vector<Value> &key_values) const;
    int CompareKeys(const std::vector<char> &lhs, const std::vector<char> &rhs) const;

    PageNo FindLeafPageNoEncoded(const std::vector<char> &encoded_key) const;
    SplitResult InsertRecursive(PageNo page_no,
                                const std::vector<char> &encoded_key,
                                const RID &rid,
                                const TransactionPtr &txn);
    DeleteResult DeleteRecursive(PageNo page_no,
                                 const std::vector<char> &encoded_key,
                                 const RID &rid,
                                 const TransactionPtr &txn,
                                 bool is_root);

    std::optional<std::vector<char>> GetSubtreeFirstKey(PageNo page_no) const;
    void NormalizeReadCursor(PageId &pid, Page *&page) const;
    bool CanFitLeafEntries(const std::vector<GenericLeafEntry> &entries) const;
    bool CanFitInternalEntries(PageNo leftmost_child,
                               const std::vector<GenericInternalEntry> &entries) const;
    bool IsLeafUnderfull(const std::vector<GenericLeafEntry> &entries) const;
    bool IsInternalUnderfull(const std::vector<GenericInternalEntry> &entries) const;
    PageNo ChildAt(const std::vector<GenericInternalEntry> &entries,
                   PageNo leftmost_child,
                   std::size_t child_index) const;
    int FindChildPosition(const std::vector<GenericInternalEntry> &entries,
                          PageNo leftmost_child,
                          PageNo child_page_no) const;

private:
    BufferPoolManager *buffer_pool_manager_;
    IndexDefinition definition_;
    uint32_t max_key_bytes_;
    LogManager *log_manager_;
};

}  // namespace simpledb
