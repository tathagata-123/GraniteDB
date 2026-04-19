// B+ tree insertion logic, including split propagation and public insert entry points.

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

BTreeIndex::SplitResult BTreeIndex::InsertRecursive(PageNo page_no,
                                                    const std::vector<char> &encoded_key,
                                                    const RID &rid,
                                                    const TransactionPtr &txn) {
    PageId pid{index_relation_id_, page_no};
    Page *page = buffer_pool_manager_->FetchPage(pid);
    if (page == nullptr) throw std::runtime_error("Failed to fetch B+ tree page during insert");

    page->WLatch();
    BTreeNodePage node(page, key_type_, max_varchar_len_);

    while (node.HasHighKey() && node.GetRightLink() != INVALID_PAGE_NO &&
           IndexKeyUtil::CompareEncoded(key_type_, encoded_key, node.GetHighKey()) >= 0) {
        PageNo right = node.GetRightLink();
        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        pid = PageId{index_relation_id_, right};
        page = buffer_pool_manager_->FetchPage(pid);
        if (page == nullptr) throw std::runtime_error("Failed to move right during insert");
        page->WLatch();
        node = BTreeNodePage(page, key_type_, max_varchar_len_);
        page_no = right;
    }

    if (node.IsLeaf()) {
        std::vector<LeafEntry> entries = node.ReadLeafEntries();
        entries.push_back(LeafEntry{encoded_key, rid});
        std::sort(entries.begin(), entries.end(), [&](const LeafEntry &a, const LeafEntry &b) {
            int cmp = IndexKeyUtil::CompareEncoded(key_type_, a.key, b.key);
            if (cmp != 0) return cmp < 0;
            if (a.rid.page_no != b.rid.page_no) return a.rid.page_no < b.rid.page_no;
            return a.rid.slot_no < b.rid.slot_no;
        });

        PageNo old_right = node.GetRightLink();
        std::vector<char> old_high_key;
        bool had_old_high_key = node.HasHighKey();
        if (had_old_high_key) old_high_key = node.GetHighKey();

        std::vector<char> before_left(page->GetData(), page->GetData() + PAGE_SIZE);
        if (node.RewriteLeaf(entries, old_right, had_old_high_key ? &old_high_key : nullptr)) {
            std::vector<char> after_left(page->GetData(), page->GetData() + PAGE_SIZE);
            LogPageChange(txn, LogRecordType::BTREE_INSERT, page, pid, before_left, after_left);
            page->WUnlatch();
            buffer_pool_manager_->UnpinPage(pid, true);
            return {};
        }

        PageId new_pid{};
        Page *new_page = buffer_pool_manager_->NewPage(index_relation_id_, &new_pid);
        if (new_page == nullptr) {
            page->WUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            throw std::runtime_error("Buffer pool full during leaf split");
        }

        new_page->WLatch();
        BTreeNodePage right_node(new_page, key_type_, max_varchar_len_);
        std::size_t mid = entries.size() / 2;
        std::vector<LeafEntry> left_entries(entries.begin(), entries.begin() + mid);
        std::vector<LeafEntry> right_entries(entries.begin() + mid, entries.end());
        std::vector<char> separator = right_entries.front().key;

        std::vector<char> before_right(new_page->GetData(), new_page->GetData() + PAGE_SIZE);
        bool ok_left = node.RewriteLeaf(left_entries, new_pid.page_no, &separator);
        bool ok_right = right_node.RewriteLeaf(right_entries, old_right, had_old_high_key ? &old_high_key : nullptr);
        std::vector<char> after_left(page->GetData(), page->GetData() + PAGE_SIZE);
        std::vector<char> after_right(new_page->GetData(), new_page->GetData() + PAGE_SIZE);
        LogPageChange(txn, LogRecordType::BTREE_PAGE_SPLIT, page, pid, before_left, after_left);
        LogPageChange(txn, LogRecordType::BTREE_PAGE_SPLIT, new_page, new_pid, before_right, after_right);

        new_page->WUnlatch();
        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(new_pid, ok_right);
        buffer_pool_manager_->UnpinPage(pid, ok_left);
        if (!ok_left || !ok_right) throw std::runtime_error("Leaf split rewrite failed");

        SplitResult split; split.did_split = true; split.separator_key = separator; split.right_page_no = new_pid.page_no; return split;
    }

    PageNo leftmost_child = node.GetLeftmostChild();
    std::vector<InternalEntry> entries = node.ReadInternalEntries();
    PageNo old_right = node.GetRightLink();
    std::vector<char> old_high_key;
    bool had_old_high_key = node.HasHighKey();
    if (had_old_high_key) old_high_key = node.GetHighKey();

    PageNo child_page_no = node.FindChildForKey(encoded_key);
    page->WUnlatch();
    SplitResult child_split = InsertRecursive(child_page_no, encoded_key, rid, txn);
    page->WLatch();
    node = BTreeNodePage(page, key_type_, max_varchar_len_);
    if (!child_split.did_split) {
        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        return {};
    }

    entries.push_back(InternalEntry{child_split.separator_key, child_split.right_page_no});
    std::sort(entries.begin(), entries.end(), [&](const InternalEntry &a, const InternalEntry &b) {
        return IndexKeyUtil::CompareEncoded(key_type_, a.key, b.key) < 0;
    });

    std::vector<char> before_left(page->GetData(), page->GetData() + PAGE_SIZE);
    if (node.RewriteInternal(leftmost_child, entries, old_right, had_old_high_key ? &old_high_key : nullptr)) {
        std::vector<char> after_left(page->GetData(), page->GetData() + PAGE_SIZE);
        LogPageChange(txn, LogRecordType::BTREE_INSERT, page, pid, before_left, after_left);
        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(pid, true);
        return {};
    }

    PageId new_pid{};
    Page *new_page = buffer_pool_manager_->NewPage(index_relation_id_, &new_pid);
    if (new_page == nullptr) {
        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        throw std::runtime_error("Buffer pool full during internal split");
    }

    new_page->WLatch();
    BTreeNodePage right_node(new_page, key_type_, max_varchar_len_);
    std::size_t mid = entries.size() / 2;
    std::vector<char> promoted_key = entries[mid].key;
    std::vector<InternalEntry> left_entries(entries.begin(), entries.begin() + mid);
    PageNo right_leftmost_child = entries[mid].child;
    std::vector<InternalEntry> right_entries(entries.begin() + mid + 1, entries.end());

    std::vector<char> before_right(new_page->GetData(), new_page->GetData() + PAGE_SIZE);
    bool ok_left = node.RewriteInternal(leftmost_child, left_entries, new_pid.page_no, &promoted_key);
    bool ok_right = right_node.RewriteInternal(right_leftmost_child, right_entries, old_right, had_old_high_key ? &old_high_key : nullptr);
    std::vector<char> after_left(page->GetData(), page->GetData() + PAGE_SIZE);
    std::vector<char> after_right(new_page->GetData(), new_page->GetData() + PAGE_SIZE);
    LogPageChange(txn, LogRecordType::BTREE_PAGE_SPLIT, page, pid, before_left, after_left);
    LogPageChange(txn, LogRecordType::BTREE_PAGE_SPLIT, new_page, new_pid, before_right, after_right);

    new_page->WUnlatch();
    page->WUnlatch();
    buffer_pool_manager_->UnpinPage(new_pid, ok_right);
    buffer_pool_manager_->UnpinPage(pid, ok_left);
    if (!ok_left || !ok_right) throw std::runtime_error("Internal split rewrite failed");

    SplitResult split; split.did_split = true; split.separator_key = promoted_key; split.right_page_no = new_pid.page_no; return split;
}


void BTreeIndex::Insert(const Value &key, const RID &rid) {
    Insert(nullptr, key, rid);
}

void BTreeIndex::Insert(const TransactionPtr &txn, const Value &key, const RID &rid) {
    auto op = buffer_pool_manager_->GetPageRetireManager()->Guard();
    std::vector<char> encoded_key = IndexKeyUtil::EncodeValue(key, key_type_, max_varchar_len_);
    BTreeMetaPageData meta = ReadMetaPage();
    PageId root_pid{index_relation_id_, meta.root_page_no};
    Page *current_page = buffer_pool_manager_->FetchPage(root_pid);
    if (current_page == nullptr) throw std::runtime_error("Failed to fetch B+ tree root page during insert");
    current_page->WLatch();
    PageId current_pid = root_pid;
    std::vector<WriteLatchFrame> path;

    auto release_current = [&](bool dirty) {
        ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, dirty);
        current_page = nullptr;
    };

    auto move_right = [&](PageId &pid, Page *&page) {
        BTreeNodePage node(page, key_type_, max_varchar_len_);
        while (node.HasHighKey() && node.GetRightLink() != INVALID_PAGE_NO &&
               IndexKeyUtil::CompareEncoded(key_type_, encoded_key, node.GetHighKey()) >= 0) {
            PageId right_pid{index_relation_id_, node.GetRightLink()};
            Page *right_page = buffer_pool_manager_->FetchPage(right_pid);
            if (right_page == nullptr) {
                ReleaseWritePath(buffer_pool_manager_, path);
                ReleaseWriteFrame(buffer_pool_manager_, {pid, page}, false);
                throw std::runtime_error("Failed to move right during B+ tree insert descent");
            }
            right_page->WLatch();
            ReleaseWriteFrame(buffer_pool_manager_, {pid, page}, false);
            pid = right_pid;
            page = right_page;
            node = BTreeNodePage(page, key_type_, max_varchar_len_);
        }
    };

    auto child_safe_for_insert = [&](Page *page) {
        BTreeNodePage node(page, key_type_, max_varchar_len_);
        if (node.IsLeaf()) {
            std::vector<LeafEntry> entries = node.ReadLeafEntries();
            entries.push_back(LeafEntry{encoded_key, rid});
            std::sort(entries.begin(), entries.end(), [&](const LeafEntry &a, const LeafEntry &b) {
                int cmp = IndexKeyUtil::CompareEncoded(key_type_, a.key, b.key);
                if (cmp != 0) return cmp < 0;
                if (a.rid.page_no != b.rid.page_no) return a.rid.page_no < b.rid.page_no;
                return a.rid.slot_no < b.rid.slot_no;
            });
            return CanFitLeafEntries(entries);
        }
        std::vector<InternalEntry> entries = node.ReadInternalEntries();
        return InternalUsedBytes(entries) + MaxInternalInsertBytes(max_key_bytes_) <= NodeCapacityBytes(node.GetSpecialSize());
    };

    while (true) {
        move_right(current_pid, current_page);
        BTreeNodePage current(current_page, key_type_, max_varchar_len_);
        if (current.IsLeaf()) break;

        PageId child_pid{index_relation_id_, current.FindChildForKey(encoded_key)};
        Page *child_page = buffer_pool_manager_->FetchPage(child_pid);
        if (child_page == nullptr) {
            ReleaseWritePath(buffer_pool_manager_, path);
            release_current(false);
            throw std::runtime_error("Failed to fetch child page during B+ tree insert descent");
        }
        child_page->WLatch();
        move_right(child_pid, child_page);

        if (child_safe_for_insert(child_page)) {
            ReleaseWritePath(buffer_pool_manager_, path);
            release_current(false);
        } else {
            path.push_back({current_pid, current_page});
        }
        current_pid = child_pid;
        current_page = child_page;
    }

    auto rewrite_leaf = [&](PageId pid, Page *page, const std::vector<LeafEntry> &entries, PageNo right_link,
                            const std::vector<char> *high_key_or_null, LogRecordType type) {
        std::vector<char> before(page->GetData(), page->GetData() + PAGE_SIZE);
        BTreeNodePage node(page, key_type_, max_varchar_len_);
        assert(!node.IsRetired());
        if (!node.RewriteLeaf(entries, right_link, high_key_or_null)) {
            ReleaseWritePath(buffer_pool_manager_, path);
            ReleaseWriteFrame(buffer_pool_manager_, {pid, page}, false);
            throw std::runtime_error("Failed to rewrite B+ tree leaf page");
        }
        std::vector<char> after(page->GetData(), page->GetData() + PAGE_SIZE);
        LogPageChange(txn, type, page, pid, before, after);
    };

    auto rewrite_internal = [&](PageId pid, Page *page, PageNo leftmost_child, const std::vector<InternalEntry> &entries,
                                PageNo right_link, const std::vector<char> *high_key_or_null, LogRecordType type) {
        std::vector<char> before(page->GetData(), page->GetData() + PAGE_SIZE);
        BTreeNodePage node(page, key_type_, max_varchar_len_);
        assert(!node.IsRetired());
        if (!node.RewriteInternal(leftmost_child, entries, right_link, high_key_or_null)) {
            ReleaseWritePath(buffer_pool_manager_, path);
            ReleaseWriteFrame(buffer_pool_manager_, {pid, page}, false);
            throw std::runtime_error("Failed to rewrite B+ tree internal page");
        }
        std::vector<char> after(page->GetData(), page->GetData() + PAGE_SIZE);
        LogPageChange(txn, type, page, pid, before, after);
    };

    BTreeNodePage leaf_node(current_page, key_type_, max_varchar_len_);
    std::vector<LeafEntry> leaf_entries = leaf_node.ReadLeafEntries();
    leaf_entries.push_back(LeafEntry{encoded_key, rid});
    std::sort(leaf_entries.begin(), leaf_entries.end(), [&](const LeafEntry &a, const LeafEntry &b) {
        int cmp = IndexKeyUtil::CompareEncoded(key_type_, a.key, b.key);
        if (cmp != 0) return cmp < 0;
        if (a.rid.page_no != b.rid.page_no) return a.rid.page_no < b.rid.page_no;
        return a.rid.slot_no < b.rid.slot_no;
    });

    PageNo old_right = leaf_node.GetRightLink();
    std::vector<char> old_high_key;
    bool had_old_high_key = leaf_node.HasHighKey();
    if (had_old_high_key) old_high_key = leaf_node.GetHighKey();

    SplitResult pending;
    PageNo pending_left_page_no = INVALID_PAGE_NO;
    if (CanFitLeafEntries(leaf_entries)) {
        rewrite_leaf(current_pid, current_page, leaf_entries, old_right, had_old_high_key ? &old_high_key : nullptr,
                     LogRecordType::BTREE_INSERT);
        ReleaseWritePath(buffer_pool_manager_, path);
        ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, true);
        return;
    }

    {
        PageId new_pid{};
        Page *new_page = buffer_pool_manager_->NewPage(index_relation_id_, &new_pid);
        if (new_page == nullptr) {
            ReleaseWritePath(buffer_pool_manager_, path);
            ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, false);
            throw std::runtime_error("Buffer pool full during B+ tree leaf split");
        }
        new_page->WLatch();

        std::size_t mid = leaf_entries.size() / 2;
        std::vector<LeafEntry> left_entries(leaf_entries.begin(), leaf_entries.begin() + mid);
        std::vector<LeafEntry> right_entries(leaf_entries.begin() + mid, leaf_entries.end());
        std::vector<char> separator = right_entries.front().key;

        rewrite_leaf(current_pid, current_page, left_entries, new_pid.page_no, &separator, LogRecordType::BTREE_PAGE_SPLIT);
        rewrite_leaf(new_pid, new_page, right_entries, old_right, had_old_high_key ? &old_high_key : nullptr,
                     LogRecordType::BTREE_PAGE_SPLIT);

        new_page->WUnlatch();
        buffer_pool_manager_->UnpinPage(new_pid, true);
        ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, true);
        pending.did_split = true;
        pending.separator_key = separator;
        pending.right_page_no = new_pid.page_no;
        pending_left_page_no = current_pid.page_no;
    }

    while (pending.did_split) {
        if (path.empty()) {
            PageId new_root_pid{};
            Page *new_root_page = buffer_pool_manager_->NewPage(index_relation_id_, &new_root_pid);
            if (new_root_page == nullptr) {
                throw std::runtime_error("Buffer pool full during B+ tree root split");
            }
            new_root_page->WLatch();
            std::vector<InternalEntry> root_entries{InternalEntry{pending.separator_key, pending.right_page_no}};
            std::vector<char> before_root(new_root_page->GetData(), new_root_page->GetData() + PAGE_SIZE);
            BTreeNodePage root_node(new_root_page, key_type_, max_varchar_len_);
            bool ok = root_node.RewriteInternal(pending_left_page_no, root_entries, INVALID_PAGE_NO, nullptr);
            if (!ok) {
                new_root_page->WUnlatch();
                buffer_pool_manager_->UnpinPage(new_root_pid, false);
                throw std::runtime_error("Failed to build new B+ tree root");
            }
            std::vector<char> after_root(new_root_page->GetData(), new_root_page->GetData() + PAGE_SIZE);
            LogPageChange(txn, LogRecordType::BTREE_PAGE_SPLIT, new_root_page, new_root_pid, before_root, after_root);
            new_root_page->WUnlatch();
            buffer_pool_manager_->UnpinPage(new_root_pid, true);
            BTreeMetaPageData meta2 = ReadMetaPage();
            meta2.root_page_no = new_root_pid.page_no;
            meta2.tree_height++;
            WriteMetaPage(meta2, txn);
            return;
        }

        WriteLatchFrame parent_frame = path.back();
        path.pop_back();
        current_pid = parent_frame.pid;
        current_page = parent_frame.page;
        BTreeNodePage parent_node(current_page, key_type_, max_varchar_len_);

        PageNo parent_leftmost_child = parent_node.GetLeftmostChild();
        std::vector<InternalEntry> parent_entries = parent_node.ReadInternalEntries();
        parent_entries.push_back(InternalEntry{pending.separator_key, pending.right_page_no});
        std::sort(parent_entries.begin(), parent_entries.end(), [&](const InternalEntry &a, const InternalEntry &b) {
            return IndexKeyUtil::CompareEncoded(key_type_, a.key, b.key) < 0;
        });

        if (CanFitInternalEntries(parent_leftmost_child, parent_entries)) {
            PageNo parent_right = parent_node.GetRightLink();
            std::vector<char> parent_high_key;
            bool parent_has_high_key = parent_node.HasHighKey();
            if (parent_has_high_key) parent_high_key = parent_node.GetHighKey();
            rewrite_internal(current_pid, current_page, parent_leftmost_child, parent_entries, parent_right,
                             parent_has_high_key ? &parent_high_key : nullptr, LogRecordType::BTREE_INSERT);
            ReleaseWritePath(buffer_pool_manager_, path);
            ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, true);
            return;
        }

        PageNo parent_right = parent_node.GetRightLink();
        std::vector<char> parent_high_key;
        bool parent_has_high_key = parent_node.HasHighKey();
        if (parent_has_high_key) parent_high_key = parent_node.GetHighKey();

        PageId new_pid{};
        Page *new_page = buffer_pool_manager_->NewPage(index_relation_id_, &new_pid);
        if (new_page == nullptr) {
            ReleaseWritePath(buffer_pool_manager_, path);
            ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, false);
            throw std::runtime_error("Buffer pool full during B+ tree internal split");
        }
        new_page->WLatch();

        std::size_t mid = parent_entries.size() / 2;
        std::vector<char> promoted_key = parent_entries[mid].key;
        std::vector<InternalEntry> left_entries(parent_entries.begin(), parent_entries.begin() + mid);
        PageNo right_leftmost_child = parent_entries[mid].child;
        std::vector<InternalEntry> right_entries(parent_entries.begin() + mid + 1, parent_entries.end());

        rewrite_internal(current_pid, current_page, parent_leftmost_child, left_entries, new_pid.page_no, &promoted_key,
                         LogRecordType::BTREE_PAGE_SPLIT);
        rewrite_internal(new_pid, new_page, right_leftmost_child, right_entries, parent_right,
                         parent_has_high_key ? &parent_high_key : nullptr, LogRecordType::BTREE_PAGE_SPLIT);

        new_page->WUnlatch();
        buffer_pool_manager_->UnpinPage(new_pid, true);
        ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, true);

        pending.separator_key = promoted_key;
        pending.right_page_no = new_pid.page_no;
        pending_left_page_no = current_pid.page_no;
    }
}

}  // namespace simpledb
