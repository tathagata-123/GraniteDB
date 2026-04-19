#include "generic_btree_helpers.h"

#include <cassert>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>

#include "../execution/expressions.h"
#include "../recovery/wal_records.h"
#include "../storage/page_lsn_util.h"

namespace simpledb {
using namespace generic_btree_helpers;

// Core tree bootstrap and low-level page-navigation utilities live here. The
// goal is that a reader can understand the persistent shape of the tree first,
// before reading the recursive insert/delete algorithms.

GenericBTreeIndex::GenericBTreeIndex(BufferPoolManager *buffer_pool_manager,
                                     IndexDefinition definition,
                                     LogManager *log_manager)
    : buffer_pool_manager_(buffer_pool_manager),
      definition_(std::move(definition)),
      max_key_bytes_(CompositeKeyCodec::GetMaxEncodedKeySize(definition_.key_columns)),
      log_manager_(log_manager != nullptr ? log_manager : (buffer_pool_manager != nullptr ? buffer_pool_manager->GetLogManager() : nullptr)) {
    if (buffer_pool_manager_ == nullptr) throw std::runtime_error("GenericBTreeIndex requires buffer pool manager");
    InitializeIfNeeded();
}

void GenericBTreeIndex::LogPageChange(const TransactionPtr &txn,
                                      LogRecordType type,
                                      Page *page,
                                      PageId page_id,
                                      const std::vector<char> &before_image,
                                      const std::vector<char> &after_image) const {
    if (txn == nullptr || log_manager_ == nullptr) return;
    LogRecord rec = MakePageImageLogRecord(
        type, txn->GetTransactionId(), txn->GetLastLSN(), page_id, before_image, after_image);
    LSN lsn = log_manager_->AppendRecord(rec);
    txn->SetLastLSN(lsn);
    SetPageLSN(page, lsn);
}

std::vector<char> GenericBTreeIndex::EncodeKey(const std::vector<Value> &key_values) const {
    return CompositeKeyCodec::EncodeKey(key_values, definition_.key_columns, definition_.null_policy);
}

int GenericBTreeIndex::CompareKeys(const std::vector<char> &lhs, const std::vector<char> &rhs) const {
    return CompositeKeyCodec::CompareEncoded(lhs, rhs, definition_.key_columns, definition_.null_policy);
}

void GenericBTreeIndex::InitializeIfNeeded() {
    uint32_t num_pages = buffer_pool_manager_->GetDiskManager()->GetNumPages(definition_.index_relation_id);
    if (num_pages == 0) {
        PageId meta_pid{};
        Page *meta_page = buffer_pool_manager_->NewPage(definition_.index_relation_id, &meta_pid);
        if (meta_page == nullptr || meta_pid.page_no != 0) throw std::runtime_error("Failed to allocate generic B+ tree metapage");
        PageId root_pid{};
        Page *root_page = buffer_pool_manager_->NewPage(definition_.index_relation_id, &root_pid);
        if (root_page == nullptr || root_pid.page_no != 1) throw std::runtime_error("Failed to allocate generic B+ tree root page");

        root_page->WLatch();
        { GenericBTreeNodePage root(root_page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b) { return CompareKeys(a, b); }); root.InitializeLeaf(); }
        root_page->WUnlatch();

        BTreeMetaPageData meta{};
        meta.page_lsn = 0;
        meta.magic = BTREE_MAGIC;
        meta.root_page_no = root_pid.page_no;
        meta.tree_height = 1;
        meta.key_type = TypeId::VARCHAR;
        meta.max_varchar_len = max_key_bytes_;
        meta.max_key_bytes = max_key_bytes_;
        meta.reserved = 0;

        meta_page->WLatch();
        std::memset(meta_page->GetData(), 0, PAGE_SIZE);
        std::memcpy(meta_page->GetData(), &meta, sizeof(meta));
        meta_page->WUnlatch();

        definition_.root_page_no = root_pid.page_no;
        buffer_pool_manager_->UnpinPage(meta_pid, true);
        buffer_pool_manager_->UnpinPage(root_pid, true);
        return;
    }

    BTreeMetaPageData meta = ReadMetaPage();
    if (meta.magic != BTREE_MAGIC) throw std::runtime_error("Invalid generic B+ tree metapage");
    definition_.root_page_no = meta.root_page_no;
}

BTreeMetaPageData GenericBTreeIndex::ReadMetaPage() const {
    PageId meta_pid{definition_.index_relation_id, 0};
    Page *page = buffer_pool_manager_->FetchPage(meta_pid);
    if (page == nullptr) throw std::runtime_error("Failed to fetch generic B+ tree metapage");
    page->RLatch();
    BTreeMetaPageData meta{};
    std::memcpy(&meta, page->GetData(), sizeof(meta));
    page->RUnlatch();
    buffer_pool_manager_->UnpinPage(meta_pid, false);
    return meta;
}

void GenericBTreeIndex::WriteMetaPage(const BTreeMetaPageData &meta, const TransactionPtr &txn) {
    PageId meta_pid{definition_.index_relation_id, 0};
    Page *page = buffer_pool_manager_->FetchPage(meta_pid);
    if (page == nullptr) throw std::runtime_error("Failed to fetch generic B+ tree metapage for write");
    page->WLatch();
    std::vector<char> before(page->GetData(), page->GetData() + PAGE_SIZE);
    std::memset(page->GetData(), 0, PAGE_SIZE);
    std::memcpy(page->GetData(), &meta, sizeof(meta));
    std::vector<char> after(page->GetData(), page->GetData() + PAGE_SIZE);
    LogPageChange(txn, LogRecordType::BTREE_META_UPDATE, page, meta_pid, before, after);
    page->WUnlatch();
    buffer_pool_manager_->UnpinPage(meta_pid, true);
}

PageNo GenericBTreeIndex::GetRootPageNo() const { auto op = buffer_pool_manager_->GetPageRetireManager()->Guard(); return ReadMetaPage().root_page_no; }

bool GenericBTreeIndex::CanFitLeafEntries(const std::vector<GenericLeafEntry> &entries) const {
    Page scratch;
    GenericBTreeNodePage node(&scratch, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
    return node.RewriteLeaf(entries, INVALID_PAGE_NO, nullptr);
}

bool GenericBTreeIndex::CanFitInternalEntries(PageNo leftmost_child,
                                              const std::vector<GenericInternalEntry> &entries) const {
    Page scratch;
    GenericBTreeNodePage node(&scratch, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
    return node.RewriteInternal(leftmost_child, entries, INVALID_PAGE_NO, nullptr);
}

bool GenericBTreeIndex::IsLeafUnderfull(const std::vector<GenericLeafEntry> &entries) const {
    Page scratch;
    GenericBTreeNodePage node(&scratch, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
    if (!node.RewriteLeaf(entries, INVALID_PAGE_NO, nullptr)) throw std::runtime_error("Leaf entries do not fit in scratch page");
    uint32_t capacity = NodeCapacityBytes(node.GetSpecialSize());
    return LeafUsedBytes(entries) < (capacity / 2);
}

bool GenericBTreeIndex::IsInternalUnderfull(const std::vector<GenericInternalEntry> &entries) const {
    Page scratch;
    GenericBTreeNodePage node(&scratch, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
    if (!node.RewriteInternal(0, entries, INVALID_PAGE_NO, nullptr)) throw std::runtime_error("Internal entries do not fit in scratch page");
    uint32_t capacity = NodeCapacityBytes(node.GetSpecialSize());
    return InternalUsedBytes(entries) < (capacity / 2);
}

PageNo GenericBTreeIndex::ChildAt(const std::vector<GenericInternalEntry> &entries,
                                  PageNo leftmost_child,
                                  std::size_t child_index) const {
    if (child_index == 0) return leftmost_child;
    if (child_index - 1 >= entries.size()) throw std::runtime_error("Child index out of range");
    return entries[child_index - 1].child;
}

int GenericBTreeIndex::FindChildPosition(const std::vector<GenericInternalEntry> &entries,
                                         PageNo leftmost_child,
                                         PageNo child_page_no) const {
    if (leftmost_child == child_page_no) return 0;
    for (std::size_t i = 0; i < entries.size(); ++i) if (entries[i].child == child_page_no) return static_cast<int>(i + 1);
    return -1;
}

void GenericBTreeIndex::NormalizeReadCursor(PageId &pid, Page *&page) const {
    while (true) {
        GenericBTreeNodePage node(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
        if (!node.IsRetired()) return;

        PageNo redirect = INVALID_PAGE_NO;
        if (node.IsLeaf()) {
            if (node.GetLeftmostChild() != INVALID_PAGE_NO) redirect = node.GetLeftmostChild();
            else redirect = node.GetRightLink();
        } else {
            redirect = node.GetLeftmostChild();
        }

        if (redirect == INVALID_PAGE_NO) {
            page->RUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            throw std::runtime_error("Encountered retired generic B+ tree page without redirect target during read descent");
        }

        PageId next_pid{definition_.index_relation_id, redirect};
        Page *next_page = buffer_pool_manager_->FetchPage(next_pid);
        if (next_page == nullptr) {
            page->RUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            throw std::runtime_error("Failed to follow retired generic B+ tree redirect during read descent");
        }
        next_page->RLatch();
        page->RUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        pid = next_pid;
        page = next_page;
    }
}

std::optional<std::vector<char>> GenericBTreeIndex::GetSubtreeFirstKey(PageNo page_no) const {
    auto op = buffer_pool_manager_->GetPageRetireManager()->Guard();
    PageId pid{definition_.index_relation_id, page_no};
    Page *page = buffer_pool_manager_->FetchPage(pid);
    if (page == nullptr) throw std::runtime_error("Failed to fetch page while reading subtree first key");
    page->RLatch();
    NormalizeReadCursor(pid, page);

    while (true) {
        GenericBTreeNodePage node(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
        if (node.IsLeaf()) {
            std::vector<GenericLeafEntry> entries = node.ReadLeafEntries();
            page->RUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            if (entries.empty()) return std::nullopt;
            return entries.front().key;
        }
        PageId child_pid{definition_.index_relation_id, node.GetLeftmostChild()};
        Page *child_page = buffer_pool_manager_->FetchPage(child_pid);
        if (child_page == nullptr) {
            page->RUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            throw std::runtime_error("Failed to fetch child page while reading subtree first key");
        }
        child_page->RLatch();
        NormalizeReadCursor(child_pid, child_page);
        page->RUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        pid = child_pid;
        page = child_page;
    }
}

PageNo GenericBTreeIndex::FindLeafPageNoEncoded(const std::vector<char> &encoded_key) const {
    auto op = buffer_pool_manager_->GetPageRetireManager()->Guard();
    BTreeMetaPageData meta = ReadMetaPage();
    PageId pid{definition_.index_relation_id, meta.root_page_no};
    Page *page = buffer_pool_manager_->FetchPage(pid);
    if (page == nullptr) throw std::runtime_error("Failed to fetch generic B+ tree page during descent");
    page->RLatch();
    NormalizeReadCursor(pid, page);

    while (true) {
        GenericBTreeNodePage node(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });

        while (node.HasHighKey() && node.GetRightLink() != INVALID_PAGE_NO && CompareKeys(encoded_key, node.GetHighKey()) >= 0) {
            PageId right_pid{definition_.index_relation_id, node.GetRightLink()};
            Page *right_page = buffer_pool_manager_->FetchPage(right_pid);
            if (right_page == nullptr) {
                page->RUnlatch();
                buffer_pool_manager_->UnpinPage(pid, false);
                throw std::runtime_error("Failed to move right during descent");
            }
            right_page->RLatch();
            page->RUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            pid = right_pid;
            page = right_page;
            NormalizeReadCursor(pid, page);
            node = GenericBTreeNodePage(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
        }

        if (node.IsLeaf()) {
            PageNo leaf_page_no = pid.page_no;
            page->RUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            return leaf_page_no;
        }

        PageId child_pid{definition_.index_relation_id, node.FindChildForKey(encoded_key)};
        Page *child_page = buffer_pool_manager_->FetchPage(child_pid);
        if (child_page == nullptr) {
            page->RUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            throw std::runtime_error("Failed to fetch child during descent");
        }
        child_page->RLatch();
        NormalizeReadCursor(child_pid, child_page);
        page->RUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        pid = child_pid;
        page = child_page;
    }
}
}  // namespace simpledb
