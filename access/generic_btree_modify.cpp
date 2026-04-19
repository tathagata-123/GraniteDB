#include "generic_btree_helpers.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <optional>
#include <stdexcept>

#include "../execution/expressions.h"
#include "../recovery/wal_records.h"
#include "../storage/page_lsn_util.h"

namespace simpledb {
using namespace generic_btree_helpers;

// Structural modification paths for the composite-key B+ tree: descent, split, merge, borrow, and root updates.

GenericBTreeIndex::SplitResult GenericBTreeIndex::InsertRecursive(PageNo page_no,
                                                                  const std::vector<char> &encoded_key,
                                                                  const RID &rid,
                                                                  const TransactionPtr &txn) {
    PageId pid{definition_.index_relation_id, page_no};
    Page *page = buffer_pool_manager_->FetchPage(pid);
    if (page == nullptr) throw std::runtime_error("Failed to fetch generic B+ tree page during insert");

    page->WLatch();
    GenericBTreeNodePage node(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });

    while (node.HasHighKey() && node.GetRightLink() != INVALID_PAGE_NO && CompareKeys(encoded_key, node.GetHighKey()) >= 0) {
        PageNo right = node.GetRightLink();
        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        pid = PageId{definition_.index_relation_id, right};
        page = buffer_pool_manager_->FetchPage(pid);
        if (page == nullptr) throw std::runtime_error("Failed to move right during insert");
        page->WLatch();
        node = GenericBTreeNodePage(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
        page_no = right;
    }

    if (node.IsLeaf()) {
        std::vector<GenericLeafEntry> entries = node.ReadLeafEntries();
        entries.push_back(GenericLeafEntry{encoded_key, rid});
        std::sort(entries.begin(), entries.end(), [&](const GenericLeafEntry &a, const GenericLeafEntry &b) {
            int cmp = CompareKeys(a.key, b.key);
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
        Page *new_page = buffer_pool_manager_->NewPage(definition_.index_relation_id, &new_pid);
        if (new_page == nullptr) { page->WUnlatch(); buffer_pool_manager_->UnpinPage(pid, false); throw std::runtime_error("Buffer pool full during leaf split"); }

        new_page->WLatch();
        GenericBTreeNodePage right_node(new_page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
        std::size_t mid = entries.size() / 2;
        std::vector<GenericLeafEntry> left_entries(entries.begin(), entries.begin() + mid);
        std::vector<GenericLeafEntry> right_entries(entries.begin() + mid, entries.end());
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
    std::vector<GenericInternalEntry> entries = node.ReadInternalEntries();
    PageNo old_right = node.GetRightLink();
    std::vector<char> old_high_key;
    bool had_old_high_key = node.HasHighKey();
    if (had_old_high_key) old_high_key = node.GetHighKey();

    PageNo child_page_no = node.FindChildForKey(encoded_key);
    page->WUnlatch();
    SplitResult child_split = InsertRecursive(child_page_no, encoded_key, rid, txn);
    page->WLatch();
    node = GenericBTreeNodePage(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
    if (!child_split.did_split) { page->WUnlatch(); buffer_pool_manager_->UnpinPage(pid, false); return {}; }

    entries.push_back(GenericInternalEntry{child_split.separator_key, child_split.right_page_no});
    std::sort(entries.begin(), entries.end(), [&](const GenericInternalEntry &a, const GenericInternalEntry &b){ return CompareKeys(a.key, b.key) < 0; });

    std::vector<char> before_left(page->GetData(), page->GetData() + PAGE_SIZE);
    if (node.RewriteInternal(leftmost_child, entries, old_right, had_old_high_key ? &old_high_key : nullptr)) {
        std::vector<char> after_left(page->GetData(), page->GetData() + PAGE_SIZE);
        LogPageChange(txn, LogRecordType::BTREE_INSERT, page, pid, before_left, after_left);
        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(pid, true);
        return {};
    }

    PageId new_pid{};
    Page *new_page = buffer_pool_manager_->NewPage(definition_.index_relation_id, &new_pid);
    if (new_page == nullptr) { page->WUnlatch(); buffer_pool_manager_->UnpinPage(pid, false); throw std::runtime_error("Buffer pool full during internal split"); }

    new_page->WLatch();
    GenericBTreeNodePage right_node(new_page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
    std::size_t mid = entries.size() / 2;
    std::vector<char> promoted_key = entries[mid].key;
    std::vector<GenericInternalEntry> left_entries(entries.begin(), entries.begin() + mid);
    PageNo right_leftmost_child = entries[mid].child;
    std::vector<GenericInternalEntry> right_entries(entries.begin() + mid + 1, entries.end());

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

GenericBTreeIndex::DeleteResult GenericBTreeIndex::DeleteRecursive(PageNo page_no,
                                                                   const std::vector<char> &encoded_key,
                                                                   const RID &rid,
                                                                   const TransactionPtr &txn,
                                                                   bool is_root) {
    PageId pid{definition_.index_relation_id, page_no};
    Page *page = buffer_pool_manager_->FetchPage(pid);
    if (page == nullptr) throw std::runtime_error("Failed to fetch generic B+ tree page during delete");

    page->WLatch();
    GenericBTreeNodePage node(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });

    while (node.HasHighKey() && node.GetRightLink() != INVALID_PAGE_NO && CompareKeys(encoded_key, node.GetHighKey()) >= 0) {
        PageNo right = node.GetRightLink();
        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        pid = PageId{definition_.index_relation_id, right};
        page = buffer_pool_manager_->FetchPage(pid);
        if (page == nullptr) throw std::runtime_error("Failed to move right during delete");
        page->WLatch();
        node = GenericBTreeNodePage(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
        page_no = right;
    }

    DeleteResult result;

    auto schedule_retire = [&](PageId retired_pid) {
        buffer_pool_manager_->GetPageRetireManager()->RetirePage(
            retired_pid,
            KeepRetiredPageForRecovery);
    };

    auto retire_leaf_page = [&](PageId retired_pid, Page *retired_page, PageNo redirect_page_no, PageNo right_link,
                                const std::vector<char> *high_key_or_null) {
        std::vector<char> before(retired_page->GetData(), retired_page->GetData() + PAGE_SIZE);
        GenericBTreeNodePage retired_node(retired_page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
        assert(!retired_node.IsRetired());
        if (!retired_node.RewriteLeaf({}, right_link, high_key_or_null)) {
            throw std::runtime_error("Failed to rewrite retired generic B+ tree leaf page");
        }
        retired_node.SetLeftmostChild(redirect_page_no);
        retired_node.SetPageState(BTreePageState::RETIRED);
        std::vector<char> after(retired_page->GetData(), retired_page->GetData() + PAGE_SIZE);
        LogPageChange(txn, LogRecordType::BTREE_MERGE, retired_page, retired_pid, before, after);
    };

    auto retire_internal_page = [&](PageId retired_pid, Page *retired_page, PageNo redirect_child, PageNo right_link,
                                    const std::vector<char> *high_key_or_null) {
        std::vector<char> before(retired_page->GetData(), retired_page->GetData() + PAGE_SIZE);
        GenericBTreeNodePage retired_node(retired_page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
        assert(!retired_node.IsRetired());
        if (!retired_node.RewriteInternal(redirect_child, {}, right_link, high_key_or_null)) {
            throw std::runtime_error("Failed to rewrite retired generic B+ tree internal page");
        }
        retired_node.SetPageState(BTreePageState::RETIRED);
        std::vector<char> after(retired_page->GetData(), retired_page->GetData() + PAGE_SIZE);
        LogPageChange(txn, LogRecordType::BTREE_MERGE, retired_page, retired_pid, before, after);
    };

    if (node.IsLeaf()) {
        std::vector<GenericLeafEntry> entries = node.ReadLeafEntries();
        std::vector<char> old_first_key;
        bool had_old_first_key = !entries.empty();
        if (had_old_first_key) old_first_key = entries.front().key;

        auto it = std::find_if(entries.begin(), entries.end(), [&](const GenericLeafEntry &entry) {
            return CompareKeys(entry.key, encoded_key) == 0 && entry.rid == rid;
        });
        if (it == entries.end()) {
            page->WUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            return result;
        }
        entries.erase(it);

        PageNo old_right = node.GetRightLink();
        std::vector<char> old_high_key;
        bool had_old_high_key = node.HasHighKey();
        if (had_old_high_key) old_high_key = node.GetHighKey();

        std::vector<char> before(page->GetData(), page->GetData() + PAGE_SIZE);
        if (!node.RewriteLeaf(entries, old_right, had_old_high_key ? &old_high_key : nullptr)) {
            page->WUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            throw std::runtime_error("Failed to rewrite leaf during delete");
        }
        std::vector<char> after(page->GetData(), page->GetData() + PAGE_SIZE);
        LogPageChange(txn, LogRecordType::BTREE_DELETE, page, pid, before, after);

        result.deleted = true;
        if (!entries.empty()) {
            if (!had_old_first_key || CompareKeys(old_first_key, entries.front().key) != 0) {
                result.subtree_first_key_changed = true;
                result.new_subtree_first_key = entries.front().key;
            }
        }
        result.underflow = !is_root && IsLeafUnderfull(entries);

        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(pid, true);
        return result;
    }

    std::optional<std::vector<char>> old_subtree_first_key;
    if (!is_root) old_subtree_first_key = GetSubtreeFirstKey(page_no);

    PageNo leftmost_child = node.GetLeftmostChild();
    std::vector<GenericInternalEntry> entries = node.ReadInternalEntries();
    PageNo old_right = node.GetRightLink();
    std::vector<char> old_high_key;
    bool had_old_high_key = node.HasHighKey();
    if (had_old_high_key) old_high_key = node.GetHighKey();

    PageNo child_page_no = node.FindChildForKey(encoded_key);
    int child_pos = FindChildPosition(entries, leftmost_child, child_page_no);
    if (child_pos < 0) {
        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        throw std::runtime_error("Parent did not contain target child during delete");
    }

    page->WUnlatch();
    DeleteResult child_result = DeleteRecursive(child_page_no, encoded_key, rid, txn, false);
    page->WLatch();
    node = GenericBTreeNodePage(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
    leftmost_child = node.GetLeftmostChild();
    entries = node.ReadInternalEntries();
    old_right = node.GetRightLink();
    had_old_high_key = node.HasHighKey();
    old_high_key.clear();
    if (had_old_high_key) old_high_key = node.GetHighKey();
    child_pos = FindChildPosition(entries, leftmost_child, child_page_no);

    if (!child_result.deleted) { page->WUnlatch(); buffer_pool_manager_->UnpinPage(pid, false); return result; }
    if (child_pos < 0) { page->WUnlatch(); buffer_pool_manager_->UnpinPage(pid, false); throw std::runtime_error("Child disappeared from parent during delete"); }

    bool parent_changed = false;
    if (child_result.subtree_first_key_changed && child_pos > 0) {
        entries[static_cast<std::size_t>(child_pos - 1)].key = child_result.new_subtree_first_key;
        parent_changed = true;
    }

    if (child_result.underflow) {
        std::optional<PageId> retired_page_id;
        PageId child_pid{definition_.index_relation_id, child_page_no};
        Page *child_page = buffer_pool_manager_->FetchPage(child_pid);
        if (child_page == nullptr) { page->WUnlatch(); buffer_pool_manager_->UnpinPage(pid, false); throw std::runtime_error("Failed to fetch underflowed child page"); }
        child_page->WLatch();
        GenericBTreeNodePage child_node(child_page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });

        PageNo left_sibling_page_no = INVALID_PAGE_NO;
        PageNo right_sibling_page_no = INVALID_PAGE_NO;
        if (child_pos > 0) left_sibling_page_no = ChildAt(entries, leftmost_child, static_cast<std::size_t>(child_pos - 1));
        if (static_cast<std::size_t>(child_pos + 1) < entries.size() + 1) right_sibling_page_no = ChildAt(entries, leftmost_child, static_cast<std::size_t>(child_pos + 1));

        if (child_node.IsLeaf()) {
            std::vector<GenericLeafEntry> child_entries = child_node.ReadLeafEntries();
            PageNo child_right = child_node.GetRightLink();
            std::vector<char> child_high_key;
            bool child_has_high_key = child_node.HasHighKey();
            if (child_has_high_key) child_high_key = child_node.GetHighKey();
            bool fixed = false;

            if (left_sibling_page_no != INVALID_PAGE_NO && !fixed) {
                PageId left_pid{definition_.index_relation_id, left_sibling_page_no};
                Page *left_page = buffer_pool_manager_->FetchPage(left_pid);
                if (left_page == nullptr) throw std::runtime_error("Failed to fetch left leaf sibling");
                left_page->WLatch();
                GenericBTreeNodePage left_node(left_page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
                std::vector<GenericLeafEntry> left_entries = left_node.ReadLeafEntries();
                if (!left_entries.empty()) {
                    GenericLeafEntry borrowed = left_entries.back();
                    std::vector<GenericLeafEntry> left_after = left_entries; left_after.pop_back();
                    if (!IsLeafUnderfull(left_after)) {
                        std::vector<GenericLeafEntry> child_after = child_entries; child_after.insert(child_after.begin(), borrowed);
                        std::vector<char> new_separator = child_after.front().key;
                        PageNo left_right = left_node.GetRightLink();
                        std::vector<char> left_old_high_key; bool left_has_high_key = left_node.HasHighKey(); if (left_has_high_key) left_old_high_key = left_node.GetHighKey();
                        std::vector<char> before_left(left_page->GetData(), left_page->GetData() + PAGE_SIZE); std::vector<char> before_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                        bool ok_left = left_node.RewriteLeaf(left_after, left_right, &new_separator);
                        bool ok_child = child_node.RewriteLeaf(child_after, child_right, child_has_high_key ? &child_high_key : nullptr);
                        if (!ok_left || !ok_child) throw std::runtime_error("Leaf borrow-from-left rewrite failed");
                        std::vector<char> after_left(left_page->GetData(), left_page->GetData() + PAGE_SIZE); std::vector<char> after_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                        LogPageChange(txn, LogRecordType::BTREE_REBALANCE, left_page, left_pid, before_left, after_left);
                        LogPageChange(txn, LogRecordType::BTREE_REBALANCE, child_page, child_pid, before_child, after_child);
                        entries[static_cast<std::size_t>(child_pos - 1)].key = new_separator; parent_changed = true; fixed = true;
                    }
                }
                left_page->WUnlatch(); buffer_pool_manager_->UnpinPage(left_pid, fixed);
            }

            if (right_sibling_page_no != INVALID_PAGE_NO && !fixed) {
                PageId right_pid{definition_.index_relation_id, right_sibling_page_no};
                Page *right_page = buffer_pool_manager_->FetchPage(right_pid);
                if (right_page == nullptr) throw std::runtime_error("Failed to fetch right leaf sibling");
                right_page->WLatch();
                GenericBTreeNodePage right_node(right_page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
                std::vector<GenericLeafEntry> right_entries = right_node.ReadLeafEntries();
                if (!right_entries.empty()) {
                    GenericLeafEntry borrowed = right_entries.front();
                    std::vector<GenericLeafEntry> right_after(right_entries.begin() + 1, right_entries.end());
                    if (!right_after.empty() && !IsLeafUnderfull(right_after)) {
                        std::vector<GenericLeafEntry> child_after = child_entries; child_after.push_back(borrowed);
                        std::vector<char> new_separator = right_after.front().key;
                        PageNo right_right = right_node.GetRightLink();
                        std::vector<char> right_high_key; bool right_has_high_key = right_node.HasHighKey(); if (right_has_high_key) right_high_key = right_node.GetHighKey();
                        std::vector<char> before_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE); std::vector<char> before_right(right_page->GetData(), right_page->GetData() + PAGE_SIZE);
                        bool ok_child = child_node.RewriteLeaf(child_after, child_right, &new_separator);
                        bool ok_right = right_node.RewriteLeaf(right_after, right_right, right_has_high_key ? &right_high_key : nullptr);
                        if (!ok_child || !ok_right) throw std::runtime_error("Leaf borrow-from-right rewrite failed");
                        std::vector<char> after_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE); std::vector<char> after_right(right_page->GetData(), right_page->GetData() + PAGE_SIZE);
                        LogPageChange(txn, LogRecordType::BTREE_REBALANCE, child_page, child_pid, before_child, after_child);
                        LogPageChange(txn, LogRecordType::BTREE_REBALANCE, right_page, right_pid, before_right, after_right);
                        entries[static_cast<std::size_t>(child_pos)].key = new_separator; parent_changed = true; fixed = true;
                    }
                }
                right_page->WUnlatch(); buffer_pool_manager_->UnpinPage(right_pid, fixed);
            }

            if (!fixed && left_sibling_page_no != INVALID_PAGE_NO) {
                PageId left_pid{definition_.index_relation_id, left_sibling_page_no};
                Page *left_page = buffer_pool_manager_->FetchPage(left_pid);
                if (left_page == nullptr) throw std::runtime_error("Failed to fetch left leaf sibling for merge");
                left_page->WLatch();
                GenericBTreeNodePage left_node(left_page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
                std::vector<GenericLeafEntry> left_entries = left_node.ReadLeafEntries();
                std::vector<GenericLeafEntry> merged = left_entries; merged.insert(merged.end(), child_entries.begin(), child_entries.end());
                if (!CanFitLeafEntries(merged)) throw std::runtime_error("Merged leaf does not fit in one page");
                std::vector<char> before_left(left_page->GetData(), left_page->GetData() + PAGE_SIZE);
                bool ok_left = left_node.RewriteLeaf(merged, child_right, child_has_high_key ? &child_high_key : nullptr);
                if (!ok_left) throw std::runtime_error("Leaf merge-into-left rewrite failed");
                std::vector<char> after_left(left_page->GetData(), left_page->GetData() + PAGE_SIZE);
                LogPageChange(txn, LogRecordType::BTREE_MERGE, left_page, left_pid, before_left, after_left);
                retire_leaf_page(child_pid, child_page, left_pid.page_no, child_right, child_has_high_key ? &child_high_key : nullptr);
                entries.erase(entries.begin() + (child_pos - 1)); retired_page_id = child_pid; parent_changed = true; fixed = true;
                left_page->WUnlatch(); buffer_pool_manager_->UnpinPage(left_pid, true);
            }

            if (!fixed && right_sibling_page_no != INVALID_PAGE_NO) {
                PageId right_pid{definition_.index_relation_id, right_sibling_page_no};
                Page *right_page = buffer_pool_manager_->FetchPage(right_pid);
                if (right_page == nullptr) throw std::runtime_error("Failed to fetch right leaf sibling for merge");
                right_page->WLatch();
                GenericBTreeNodePage right_node(right_page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
                std::vector<GenericLeafEntry> right_entries = right_node.ReadLeafEntries();
                std::vector<GenericLeafEntry> merged = child_entries; merged.insert(merged.end(), right_entries.begin(), right_entries.end());
                if (!CanFitLeafEntries(merged)) throw std::runtime_error("Merged leaf does not fit in one page");
                PageNo right_right = right_node.GetRightLink();
                std::vector<char> right_high_key; bool right_has_high_key = right_node.HasHighKey(); if (right_has_high_key) right_high_key = right_node.GetHighKey();
                std::vector<char> before_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                bool ok_child = child_node.RewriteLeaf(merged, right_right, right_has_high_key ? &right_high_key : nullptr);
                if (!ok_child) throw std::runtime_error("Leaf merge-with-right rewrite failed");
                std::vector<char> after_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                LogPageChange(txn, LogRecordType::BTREE_MERGE, child_page, child_pid, before_child, after_child);
                retire_leaf_page(right_pid, right_page, page_no, right_right, right_has_high_key ? &right_high_key : nullptr);
                entries.erase(entries.begin() + child_pos); retired_page_id = right_pid; parent_changed = true; fixed = true;
                right_page->WUnlatch(); buffer_pool_manager_->UnpinPage(right_pid, true);
            }

            if (!fixed) throw std::runtime_error("Failed to rebalance underflowed leaf");
        } else {
            std::vector<GenericInternalEntry> child_entries = child_node.ReadInternalEntries();
            PageNo child_leftmost = child_node.GetLeftmostChild();
            PageNo child_right = child_node.GetRightLink();
            std::vector<char> child_high_key; bool child_has_high_key = child_node.HasHighKey(); if (child_has_high_key) child_high_key = child_node.GetHighKey();
            bool fixed = false;

            if (left_sibling_page_no != INVALID_PAGE_NO && !fixed) {
                PageId left_pid{definition_.index_relation_id, left_sibling_page_no};
                Page *left_page = buffer_pool_manager_->FetchPage(left_pid);
                if (left_page == nullptr) throw std::runtime_error("Failed to fetch left internal sibling");
                left_page->WLatch();
                GenericBTreeNodePage left_node(left_page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
                std::vector<GenericInternalEntry> left_entries = left_node.ReadInternalEntries();
                if (!left_entries.empty()) {
                    GenericInternalEntry borrowed = left_entries.back();
                    std::vector<GenericInternalEntry> left_after = left_entries; left_after.pop_back();
                    if (!IsInternalUnderfull(left_after)) {
                        std::vector<GenericInternalEntry> child_after = child_entries;
                        child_after.insert(child_after.begin(), GenericInternalEntry{entries[static_cast<std::size_t>(child_pos - 1)].key, child_leftmost});
                        PageNo new_child_leftmost = borrowed.child;
                        std::vector<char> new_separator = borrowed.key;
                        PageNo left_right = left_node.GetRightLink();
                        std::vector<char> before_left(left_page->GetData(), left_page->GetData() + PAGE_SIZE); std::vector<char> before_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                        bool ok_left = left_node.RewriteInternal(left_node.GetLeftmostChild(), left_after, left_right, &new_separator);
                        bool ok_child = child_node.RewriteInternal(new_child_leftmost, child_after, child_right, child_has_high_key ? &child_high_key : nullptr);
                        if (!ok_left || !ok_child) throw std::runtime_error("Internal borrow-from-left rewrite failed");
                        std::vector<char> after_left(left_page->GetData(), left_page->GetData() + PAGE_SIZE); std::vector<char> after_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                        LogPageChange(txn, LogRecordType::BTREE_REBALANCE, left_page, left_pid, before_left, after_left);
                        LogPageChange(txn, LogRecordType::BTREE_REBALANCE, child_page, child_pid, before_child, after_child);
                        entries[static_cast<std::size_t>(child_pos - 1)].key = new_separator; parent_changed = true; fixed = true;
                    }
                }
                left_page->WUnlatch(); buffer_pool_manager_->UnpinPage(left_pid, fixed);
            }

            if (right_sibling_page_no != INVALID_PAGE_NO && !fixed) {
                PageId right_pid{definition_.index_relation_id, right_sibling_page_no};
                Page *right_page = buffer_pool_manager_->FetchPage(right_pid);
                if (right_page == nullptr) throw std::runtime_error("Failed to fetch right internal sibling");
                right_page->WLatch();
                GenericBTreeNodePage right_node(right_page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
                std::vector<GenericInternalEntry> right_entries = right_node.ReadInternalEntries();
                if (!right_entries.empty()) {
                    std::vector<GenericInternalEntry> right_after(right_entries.begin() + 1, right_entries.end());
                    if (!right_after.empty() && !IsInternalUnderfull(right_after)) {
                        PageNo borrowed_leftmost_child = right_node.GetLeftmostChild();
                        std::vector<GenericInternalEntry> child_after = child_entries;
                        child_after.push_back(GenericInternalEntry{entries[static_cast<std::size_t>(child_pos)].key, borrowed_leftmost_child});
                        std::vector<char> new_separator = right_entries.front().key;
                        PageNo new_right_leftmost = right_entries.front().child;
                        PageNo right_right = right_node.GetRightLink();
                        std::vector<char> right_high_key; bool right_has_high_key = right_node.HasHighKey(); if (right_has_high_key) right_high_key = right_node.GetHighKey();
                        std::vector<char> before_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE); std::vector<char> before_right(right_page->GetData(), right_page->GetData() + PAGE_SIZE);
                        bool ok_child = child_node.RewriteInternal(child_leftmost, child_after, child_right, &new_separator);
                        bool ok_right = right_node.RewriteInternal(new_right_leftmost, right_after, right_right, right_has_high_key ? &right_high_key : nullptr);
                        if (!ok_child || !ok_right) throw std::runtime_error("Internal borrow-from-right rewrite failed");
                        std::vector<char> after_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE); std::vector<char> after_right(right_page->GetData(), right_page->GetData() + PAGE_SIZE);
                        LogPageChange(txn, LogRecordType::BTREE_REBALANCE, child_page, child_pid, before_child, after_child);
                        LogPageChange(txn, LogRecordType::BTREE_REBALANCE, right_page, right_pid, before_right, after_right);
                        entries[static_cast<std::size_t>(child_pos)].key = new_separator; parent_changed = true; fixed = true;
                    }
                }
                right_page->WUnlatch(); buffer_pool_manager_->UnpinPage(right_pid, fixed);
            }

            if (!fixed && left_sibling_page_no != INVALID_PAGE_NO) {
                PageId left_pid{definition_.index_relation_id, left_sibling_page_no};
                Page *left_page = buffer_pool_manager_->FetchPage(left_pid);
                if (left_page == nullptr) throw std::runtime_error("Failed to fetch left internal sibling for merge");
                left_page->WLatch();
                GenericBTreeNodePage left_node(left_page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
                std::vector<GenericInternalEntry> left_entries = left_node.ReadInternalEntries();
                std::vector<GenericInternalEntry> merged = left_entries;
                merged.push_back(GenericInternalEntry{entries[static_cast<std::size_t>(child_pos - 1)].key, child_leftmost});
                merged.insert(merged.end(), child_entries.begin(), child_entries.end());
                if (!CanFitInternalEntries(left_node.GetLeftmostChild(), merged)) throw std::runtime_error("Merged internal node does not fit in one page");
                std::vector<char> before_left(left_page->GetData(), left_page->GetData() + PAGE_SIZE);
                bool ok_left = left_node.RewriteInternal(left_node.GetLeftmostChild(), merged, child_right, child_has_high_key ? &child_high_key : nullptr);
                if (!ok_left) throw std::runtime_error("Internal merge-into-left rewrite failed");
                std::vector<char> after_left(left_page->GetData(), left_page->GetData() + PAGE_SIZE);
                LogPageChange(txn, LogRecordType::BTREE_MERGE, left_page, left_pid, before_left, after_left);
                retire_internal_page(child_pid, child_page, left_pid.page_no, child_right, child_has_high_key ? &child_high_key : nullptr);
                entries.erase(entries.begin() + (child_pos - 1)); retired_page_id = child_pid; parent_changed = true; fixed = true;
                left_page->WUnlatch(); buffer_pool_manager_->UnpinPage(left_pid, true);
            }

            if (!fixed && right_sibling_page_no != INVALID_PAGE_NO) {
                PageId right_pid{definition_.index_relation_id, right_sibling_page_no};
                Page *right_page = buffer_pool_manager_->FetchPage(right_pid);
                if (right_page == nullptr) throw std::runtime_error("Failed to fetch right internal sibling for merge");
                right_page->WLatch();
                GenericBTreeNodePage right_node(right_page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
                std::vector<GenericInternalEntry> right_entries = right_node.ReadInternalEntries();
                std::vector<GenericInternalEntry> merged = child_entries;
                merged.push_back(GenericInternalEntry{entries[static_cast<std::size_t>(child_pos)].key, right_node.GetLeftmostChild()});
                merged.insert(merged.end(), right_entries.begin(), right_entries.end());
                if (!CanFitInternalEntries(child_leftmost, merged)) throw std::runtime_error("Merged internal node does not fit in one page");
                PageNo right_right = right_node.GetRightLink();
                std::vector<char> right_high_key; bool right_has_high_key = right_node.HasHighKey(); if (right_has_high_key) right_high_key = right_node.GetHighKey();
                std::vector<char> before_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                bool ok_child = child_node.RewriteInternal(child_leftmost, merged, right_right, right_has_high_key ? &right_high_key : nullptr);
                if (!ok_child) throw std::runtime_error("Internal merge-with-right rewrite failed");
                std::vector<char> after_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                LogPageChange(txn, LogRecordType::BTREE_MERGE, child_page, child_pid, before_child, after_child);
                retire_internal_page(right_pid, right_page, child_pid.page_no, right_right, right_has_high_key ? &right_high_key : nullptr);
                entries.erase(entries.begin() + child_pos); retired_page_id = right_pid; parent_changed = true; fixed = true;
                right_page->WUnlatch(); buffer_pool_manager_->UnpinPage(right_pid, true);
            }

            if (!fixed) throw std::runtime_error("Failed to rebalance underflowed internal node");
        }

        child_page->WUnlatch();
        buffer_pool_manager_->UnpinPage(child_pid, retired_page_id.has_value() && *retired_page_id == child_pid);
        if (retired_page_id.has_value()) {
            schedule_retire(*retired_page_id);
        }
    }

    result.deleted = true;

    if (is_root && entries.empty()) {
        PageNo new_root = leftmost_child;
        retire_internal_page(pid, page, new_root, INVALID_PAGE_NO, nullptr);
        BTreeMetaPageData meta = ReadMetaPage();
        if (meta.root_page_no == page_no && meta.tree_height > 1) {
            meta.root_page_no = new_root;
            meta.tree_height--;
            WriteMetaPage(meta, txn);
        }
        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(pid, true);
        schedule_retire(pid);
        return result;
    }

    std::vector<char> before(page->GetData(), page->GetData() + PAGE_SIZE);
    if (!node.RewriteInternal(leftmost_child, entries, old_right, had_old_high_key ? &old_high_key : nullptr)) {
        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        throw std::runtime_error("Failed to rewrite internal page during delete");
    }
    std::vector<char> after(page->GetData(), page->GetData() + PAGE_SIZE);
    if (parent_changed || child_result.underflow) LogPageChange(txn, LogRecordType::BTREE_REBALANCE, page, pid, before, after);

    result.underflow = !is_root && IsInternalUnderfull(entries);
    if (!is_root) {
        std::optional<std::vector<char>> new_first_key = GetSubtreeFirstKey(page_no);
        if (old_subtree_first_key.has_value() != new_first_key.has_value() ||
            (old_subtree_first_key.has_value() && CompareKeys(*old_subtree_first_key, *new_first_key) != 0)) {
            result.subtree_first_key_changed = new_first_key.has_value();
            if (new_first_key.has_value()) result.new_subtree_first_key = *new_first_key;
        }
    }

    page->WUnlatch();
    buffer_pool_manager_->UnpinPage(pid, true);
    return result;
}

void GenericBTreeIndex::InsertEntry(const std::vector<Value> &key_values, const RID &rid) { InsertEntry(nullptr, key_values, rid); }

void GenericBTreeIndex::InsertEntry(const TransactionPtr &txn,
                                    const std::vector<Value> &key_values,
                                    const RID &rid) {
    auto op = buffer_pool_manager_->GetPageRetireManager()->Guard();
    std::vector<char> encoded_key = EncodeKey(key_values);
    BTreeMetaPageData meta = ReadMetaPage();
    SplitResult split = InsertRecursive(meta.root_page_no, encoded_key, rid, txn);
    if (!split.did_split) return;

    PageId new_root_pid{};
    Page *new_root_page = buffer_pool_manager_->NewPage(definition_.index_relation_id, &new_root_pid);
    if (new_root_page == nullptr) throw std::runtime_error("Buffer pool full during root split");

    new_root_page->WLatch();
    std::vector<char> before_root(new_root_page->GetData(), new_root_page->GetData() + PAGE_SIZE);
    GenericBTreeNodePage root(new_root_page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
    std::vector<GenericInternalEntry> root_entries;
    root_entries.push_back(GenericInternalEntry{split.separator_key, split.right_page_no});
    bool ok = root.RewriteInternal(meta.root_page_no, root_entries, INVALID_PAGE_NO, nullptr);
    std::vector<char> after_root(new_root_page->GetData(), new_root_page->GetData() + PAGE_SIZE);
    LogPageChange(txn, LogRecordType::BTREE_PAGE_SPLIT, new_root_page, new_root_pid, before_root, after_root);
    new_root_page->WUnlatch();
    buffer_pool_manager_->UnpinPage(new_root_pid, ok);
    if (!ok) throw std::runtime_error("Failed to build new root during root split");

    meta.root_page_no = new_root_pid.page_no;
    meta.tree_height++;
    WriteMetaPage(meta, txn);
}

bool GenericBTreeIndex::DeleteEntry(const std::vector<Value> &key_values, const RID &rid) { return DeleteEntry(nullptr, key_values, rid); }

bool GenericBTreeIndex::DeleteEntry(const TransactionPtr &txn,
                                    const std::vector<Value> &key_values,
                                    const RID &rid) {
    auto op = buffer_pool_manager_->GetPageRetireManager()->Guard();
    std::vector<char> encoded_key = EncodeKey(key_values);
    BTreeMetaPageData meta = ReadMetaPage();
    DeleteResult result = DeleteRecursive(meta.root_page_no, encoded_key, rid, txn, true);
    return result.deleted;
}
}  // namespace simpledb
