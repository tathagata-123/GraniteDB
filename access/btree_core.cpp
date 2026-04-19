// B+ tree construction, metadata I/O, search, and navigation helpers.

#include "btree_internal.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <optional>
#include <stdexcept>

#include "../recovery/wal_records.h"
#include "../storage/page_lsn_util.h"

namespace simpledb {
using namespace btree_detail;

BTreeIndex::BTreeIndex(BufferPoolManager *buffer_pool_manager,
                       RelationId index_relation_id,
                       TypeId key_type,
                       uint32_t max_varchar_len,
                       LogManager *log_manager)
    : buffer_pool_manager_(buffer_pool_manager),
      index_relation_id_(index_relation_id),
      key_type_(key_type),
      max_varchar_len_(max_varchar_len),
      max_key_bytes_(IndexKeyUtil::MaxEncodedKeySize(key_type, max_varchar_len)),
      log_manager_(log_manager != nullptr ? log_manager : (buffer_pool_manager != nullptr ? buffer_pool_manager->GetLogManager() : nullptr)) {
    InitializeIfNeeded();
}

void BTreeIndex::LogPageChange(const TransactionPtr &txn,
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

void BTreeIndex::InitializeIfNeeded() {
    uint32_t num_pages = buffer_pool_manager_->GetDiskManager()->GetNumPages(index_relation_id_);
    if (num_pages == 0) {
        PageId meta_pid{};
        Page *meta_page = buffer_pool_manager_->NewPage(index_relation_id_, &meta_pid);
        if (meta_page == nullptr || meta_pid.page_no != 0) throw std::runtime_error("Failed to allocate B+ tree metapage");
        PageId root_pid{};
        Page *root_page = buffer_pool_manager_->NewPage(index_relation_id_, &root_pid);
        if (root_page == nullptr || root_pid.page_no != 1) throw std::runtime_error("Failed to allocate B+ tree root page");

        root_page->WLatch();
        { BTreeNodePage root(root_page, key_type_, max_varchar_len_); root.InitializeLeaf(); }
        root_page->WUnlatch();

        BTreeMetaPageData meta{};
        meta.page_lsn = 0;
        meta.magic = BTREE_MAGIC;
        meta.root_page_no = root_pid.page_no;
        meta.tree_height = 1;
        meta.key_type = key_type_;
        meta.max_varchar_len = max_varchar_len_;
        meta.max_key_bytes = max_key_bytes_;
        meta.reserved = 0;

        meta_page->WLatch();
        std::memset(meta_page->GetData(), 0, PAGE_SIZE);
        std::memcpy(meta_page->GetData(), &meta, sizeof(meta));
        meta_page->WUnlatch();

        buffer_pool_manager_->UnpinPage(meta_pid, true);
        buffer_pool_manager_->UnpinPage(root_pid, true);
        return;
    }

    BTreeMetaPageData meta = ReadMetaPage();
    if (meta.magic != BTREE_MAGIC) throw std::runtime_error("Invalid B+ tree metapage");
    if (meta.key_type != key_type_) throw std::runtime_error("B+ tree key type mismatch");
    if (meta.max_varchar_len != max_varchar_len_) throw std::runtime_error("B+ tree max_varchar_len mismatch");
    if (meta.max_key_bytes != max_key_bytes_) throw std::runtime_error("B+ tree max_key_bytes mismatch");
}

BTreeMetaPageData BTreeIndex::ReadMetaPage() const {
    PageId meta_pid{index_relation_id_, 0};
    Page *page = buffer_pool_manager_->FetchPage(meta_pid);
    if (page == nullptr) throw std::runtime_error("Failed to fetch B+ tree metapage");
    page->RLatch();
    BTreeMetaPageData meta{};
    std::memcpy(&meta, page->GetData(), sizeof(meta));
    page->RUnlatch();
    buffer_pool_manager_->UnpinPage(meta_pid, false);
    return meta;
}

void BTreeIndex::WriteMetaPage(const BTreeMetaPageData &meta, const TransactionPtr &txn) {
    PageId meta_pid{index_relation_id_, 0};
    Page *page = buffer_pool_manager_->FetchPage(meta_pid);
    if (page == nullptr) throw std::runtime_error("Failed to fetch B+ tree metapage for write");
    page->WLatch();
    std::vector<char> before(page->GetData(), page->GetData() + PAGE_SIZE);
    std::memset(page->GetData(), 0, PAGE_SIZE);
    std::memcpy(page->GetData(), &meta, sizeof(meta));
    std::vector<char> after(page->GetData(), page->GetData() + PAGE_SIZE);
    LogPageChange(txn, LogRecordType::BTREE_META_UPDATE, page, meta_pid, before, after);
    page->WUnlatch();
    buffer_pool_manager_->UnpinPage(meta_pid, true);
}

PageNo BTreeIndex::GetRootPageNo() const { auto op = buffer_pool_manager_->GetPageRetireManager()->Guard(); return ReadMetaPage().root_page_no; }
uint32_t BTreeIndex::GetTreeHeight() const { auto op = buffer_pool_manager_->GetPageRetireManager()->Guard(); return ReadMetaPage().tree_height; }

bool BTreeIndex::CanFitLeafEntries(const std::vector<LeafEntry> &entries) const {
    Page scratch;
    BTreeNodePage node(&scratch, key_type_, max_varchar_len_);
    return node.RewriteLeaf(entries, INVALID_PAGE_NO, nullptr);
}

bool BTreeIndex::CanFitInternalEntries(PageNo leftmost_child,
                                       const std::vector<InternalEntry> &entries) const {
    Page scratch;
    BTreeNodePage node(&scratch, key_type_, max_varchar_len_);
    return node.RewriteInternal(leftmost_child, entries, INVALID_PAGE_NO, nullptr);
}

bool BTreeIndex::IsLeafUnderfull(const std::vector<LeafEntry> &entries) const {
    Page scratch;
    BTreeNodePage node(&scratch, key_type_, max_varchar_len_);
    if (!node.RewriteLeaf(entries, INVALID_PAGE_NO, nullptr)) {
        throw std::runtime_error("Leaf entries do not fit in scratch page");
    }
    uint32_t used = 0;
    for (const auto &entry : entries) {
        used += static_cast<uint32_t>(sizeof(uint32_t) + entry.key.size() + sizeof(PageNo) + sizeof(SlotNo) + sizeof(SlotEntry));
    }
    uint32_t capacity = static_cast<uint32_t>(PAGE_SIZE - sizeof(SlottedPageHeader) - node.GetSpecialSize());
    return used < (capacity / 2);
}

bool BTreeIndex::IsInternalUnderfull(const std::vector<InternalEntry> &entries) const {
    Page scratch;
    BTreeNodePage node(&scratch, key_type_, max_varchar_len_);
    if (!node.RewriteInternal(0, entries, INVALID_PAGE_NO, nullptr)) {
        throw std::runtime_error("Internal entries do not fit in scratch page");
    }
    uint32_t used = 0;
    for (const auto &entry : entries) {
        used += static_cast<uint32_t>(sizeof(uint32_t) + entry.key.size() + sizeof(PageNo) + sizeof(SlotEntry));
    }
    uint32_t capacity = static_cast<uint32_t>(PAGE_SIZE - sizeof(SlottedPageHeader) - node.GetSpecialSize());
    return used < (capacity / 2);
}

PageNo BTreeIndex::ChildAt(const std::vector<InternalEntry> &entries,
                           PageNo leftmost_child,
                           std::size_t child_index) const {
    if (child_index == 0) return leftmost_child;
    if (child_index - 1 >= entries.size()) throw std::runtime_error("Child index out of range");
    return entries[child_index - 1].child;
}

int BTreeIndex::FindChildPosition(const std::vector<InternalEntry> &entries,
                                  PageNo leftmost_child,
                                  PageNo child_page_no) const {
    if (leftmost_child == child_page_no) return 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].child == child_page_no) return static_cast<int>(i + 1);
    }
    return -1;
}

void BTreeIndex::NormalizeReadCursor(PageId &pid, Page *&page) const {
    while (true) {
        BTreeNodePage node(page, key_type_, max_varchar_len_);
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
            throw std::runtime_error("Encountered retired B+ tree page without redirect target during read descent");
        }

        PageId next_pid{index_relation_id_, redirect};
        Page *next_page = buffer_pool_manager_->FetchPage(next_pid);
        if (next_page == nullptr) {
            page->RUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            throw std::runtime_error("Failed to follow retired B+ tree redirect during read descent");
        }
        next_page->RLatch();
        page->RUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        pid = next_pid;
        page = next_page;
    }
}

std::optional<std::vector<char>> BTreeIndex::GetSubtreeFirstKey(PageNo page_no) const {
    auto op = buffer_pool_manager_->GetPageRetireManager()->Guard();
    PageId pid{index_relation_id_, page_no};
    Page *page = buffer_pool_manager_->FetchPage(pid);
    if (page == nullptr) throw std::runtime_error("Failed to fetch page while reading subtree first key");
    page->RLatch();
    NormalizeReadCursor(pid, page);

    while (true) {
        BTreeNodePage node(page, key_type_, max_varchar_len_);
        if (node.IsLeaf()) {
            std::vector<LeafEntry> entries = node.ReadLeafEntries();
            page->RUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            if (entries.empty()) return std::nullopt;
            return entries.front().key;
        }

        PageNo child_page_no = node.GetLeftmostChild();
        PageId child_pid{index_relation_id_, child_page_no};
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

PageNo BTreeIndex::FindLeafPageNoEncoded(const std::vector<char> &encoded_key) const {
    auto op = buffer_pool_manager_->GetPageRetireManager()->Guard();
    BTreeMetaPageData meta = ReadMetaPage();
    PageId pid{index_relation_id_, meta.root_page_no};
    Page *page = buffer_pool_manager_->FetchPage(pid);
    if (page == nullptr) throw std::runtime_error("Failed to fetch B+ tree root page during descent");
    page->RLatch();
    NormalizeReadCursor(pid, page);

    while (true) {
        BTreeNodePage node(page, key_type_, max_varchar_len_);

        while (node.HasHighKey() && node.GetRightLink() != INVALID_PAGE_NO &&
               IndexKeyUtil::CompareEncoded(key_type_, encoded_key, node.GetHighKey()) >= 0) {
            PageId right_pid{index_relation_id_, node.GetRightLink()};
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
            node = BTreeNodePage(page, key_type_, max_varchar_len_);
        }

        if (node.IsLeaf()) {
            PageNo leaf_page_no = pid.page_no;
            page->RUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            return leaf_page_no;
        }

        PageId child_pid{index_relation_id_, node.FindChildForKey(encoded_key)};
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

PageNo BTreeIndex::FindLeafPageNo(const Value &key) const {
    std::vector<char> encoded = IndexKeyUtil::EncodeValue(key, key_type_, max_varchar_len_);
    return FindLeafPageNoEncoded(encoded);
}

PageNo BTreeIndex::GetLeftmostLeafPageNo() const {
    auto op = buffer_pool_manager_->GetPageRetireManager()->Guard();
    BTreeMetaPageData meta = ReadMetaPage();
    PageId pid{index_relation_id_, meta.root_page_no};
    Page *page = buffer_pool_manager_->FetchPage(pid);
    if (page == nullptr) throw std::runtime_error("Failed to fetch B+ tree page during leftmost descent");
    page->RLatch();
    NormalizeReadCursor(pid, page);

    while (true) {
        BTreeNodePage node(page, key_type_, max_varchar_len_);
        if (node.IsLeaf()) {
            PageNo leaf_page_no = pid.page_no;
            page->RUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            return leaf_page_no;
        }

        PageId child_pid{index_relation_id_, node.GetLeftmostChild()};
        Page *child_page = buffer_pool_manager_->FetchPage(child_pid);
        if (child_page == nullptr) {
            page->RUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            throw std::runtime_error("Failed to fetch child during leftmost descent");
        }
        child_page->RLatch();
        NormalizeReadCursor(child_pid, child_page);
        page->RUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        pid = child_pid;
        page = child_page;
    }
}

std::vector<RID> BTreeIndex::Search(const Value &key) const {
    auto op = buffer_pool_manager_->GetPageRetireManager()->Guard();
    std::vector<char> encoded = IndexKeyUtil::EncodeValue(key, key_type_, max_varchar_len_);
    PageId pid{index_relation_id_, FindLeafPageNoEncoded(encoded)};
    Page *page = buffer_pool_manager_->FetchPage(pid);
    if (page == nullptr) throw std::runtime_error("Failed to fetch leaf page during search");
    page->RLatch();
    NormalizeReadCursor(pid, page);

    std::vector<RID> result;
    while (true) {
        BTreeNodePage node(page, key_type_, max_varchar_len_);
        if (!node.IsLeaf()) {
            page->RUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            pid = PageId{index_relation_id_, FindLeafPageNoEncoded(encoded)};
            page = buffer_pool_manager_->FetchPage(pid);
            if (page == nullptr) throw std::runtime_error("Failed to refetch leaf page during search");
            page->RLatch();
            NormalizeReadCursor(pid, page);
            continue;
        }
        std::vector<LeafEntry> entries = node.ReadLeafEntries();

        bool continue_right = false;
        bool stop = false;
        for (const auto &entry : entries) {
            int cmp = IndexKeyUtil::CompareEncoded(key_type_, entry.key, encoded);
            if (cmp < 0) continue;
            if (cmp == 0) {
                result.push_back(entry.rid);
                continue_right = (node.GetRightLink() != INVALID_PAGE_NO);
                continue;
            }
            stop = true;
            break;
        }

        bool move_right = !stop && continue_right && !entries.empty() &&
                          IndexKeyUtil::CompareEncoded(key_type_, entries.back().key, encoded) == 0;
        if (!move_right) {
            page->RUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            return result;
        }

        PageId next_pid{index_relation_id_, node.GetRightLink()};
        Page *next_page = buffer_pool_manager_->FetchPage(next_pid);
        if (next_page == nullptr) {
            page->RUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            throw std::runtime_error("Failed to fetch right sibling during search");
        }
        next_page->RLatch();
        page->RUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        pid = next_pid;
        page = next_page;
        NormalizeReadCursor(pid, page);
    }
}


}  // namespace simpledb
