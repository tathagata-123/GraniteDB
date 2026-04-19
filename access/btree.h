// Public B+ tree interface.
//
// The implementation is intentionally split across `btree_core.cpp`,
// `btree_insert.cpp`, and `btree_delete.cpp` plus the small helper header
// `btree_internal.h` so metadata/search logic, insert/split logic, and
// delete/rebalance logic can be read separately.

#pragma once

#include <optional>
#include <vector>

#include "../buffer/buffer_pool_manager.h"
#include "../concurrency/transaction.h"
#include "../recovery/log_manager.h"
#include "../common/types.h"
#include "../common/value.h"
#include "btree_page.h"

namespace simpledb {

class BTreeIndex {
public:
    BTreeIndex(BufferPoolManager *buffer_pool_manager,
               RelationId index_relation_id,
               TypeId key_type,
               uint32_t max_varchar_len = 0,
               LogManager *log_manager = nullptr);

    void Insert(const Value &key, const RID &rid);
    void Insert(const TransactionPtr &txn, const Value &key, const RID &rid);

    bool Delete(const Value &key, const RID &rid);
    bool Delete(const TransactionPtr &txn, const Value &key, const RID &rid);

    std::vector<RID> Search(const Value &key) const;

    PageNo FindLeafPageNo(const Value &key) const;
    PageNo GetLeftmostLeafPageNo() const;

    PageNo GetRootPageNo() const;
    uint32_t GetTreeHeight() const;

    RelationId GetIndexRelationId() const { return index_relation_id_; }
    BufferPoolManager *GetBufferPoolManager() const { return buffer_pool_manager_; }
    TypeId GetKeyType() const { return key_type_; }
    uint32_t GetMaxVarcharLength() const { return max_varchar_len_; }
    uint32_t GetMaxKeyBytes() const { return max_key_bytes_; }

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

    bool CanFitLeafEntries(const std::vector<LeafEntry> &entries) const;
    bool CanFitInternalEntries(PageNo leftmost_child,
                               const std::vector<InternalEntry> &entries) const;
    bool IsLeafUnderfull(const std::vector<LeafEntry> &entries) const;
    bool IsInternalUnderfull(const std::vector<InternalEntry> &entries) const;

    PageNo ChildAt(const std::vector<InternalEntry> &entries,
                   PageNo leftmost_child,
                   std::size_t child_index) const;
    int FindChildPosition(const std::vector<InternalEntry> &entries,
                          PageNo leftmost_child,
                          PageNo child_page_no) const;

    BufferPoolManager *buffer_pool_manager_;
    RelationId index_relation_id_;
    TypeId key_type_;
    uint32_t max_varchar_len_;
    uint32_t max_key_bytes_;
    LogManager *log_manager_;
};

}  // namespace simpledb
