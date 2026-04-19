// Recursive B+ tree delete, rebalance, and merge logic.

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

BTreeIndex::DeleteResult BTreeIndex::DeleteRecursive(PageNo page_no,
                                                     const std::vector<char> &encoded_key,
                                                     const RID &rid,
                                                     const TransactionPtr &txn,
                                                     bool is_root) {
    PageId pid{index_relation_id_, page_no};
    Page *page = buffer_pool_manager_->FetchPage(pid);
    if (page == nullptr) throw std::runtime_error("Failed to fetch B+ tree page during delete");

    page->WLatch();
    BTreeNodePage node(page, key_type_, max_varchar_len_);

    while (node.HasHighKey() && node.GetRightLink() != INVALID_PAGE_NO &&
           IndexKeyUtil::CompareEncoded(key_type_, encoded_key, node.GetHighKey()) >= 0) {
        PageNo right = node.GetRightLink();
        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        pid = PageId{index_relation_id_, right};
        page = buffer_pool_manager_->FetchPage(pid);
        if (page == nullptr) throw std::runtime_error("Failed to move right during delete");
        page->WLatch();
        node = BTreeNodePage(page, key_type_, max_varchar_len_);
        page_no = right;
    }

    DeleteResult result;

    if (node.IsLeaf()) {
        std::vector<LeafEntry> entries = node.ReadLeafEntries();
        std::vector<char> old_first_key;
        bool had_old_first_key = !entries.empty();
        if (had_old_first_key) old_first_key = entries.front().key;

        auto it = std::find_if(entries.begin(), entries.end(), [&](const LeafEntry &entry) {
            return IndexKeyUtil::CompareEncoded(key_type_, entry.key, encoded_key) == 0 && entry.rid == rid;
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
        LogPageChange(txn, LogRecordType::BTREE_REBALANCE, page, pid, before, after);

        result.deleted = true;
        if (!entries.empty()) {
            if (!had_old_first_key || IndexKeyUtil::CompareEncoded(key_type_, old_first_key, entries.front().key) != 0) {
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
    if (!is_root) {
        old_subtree_first_key = GetSubtreeFirstKey(page_no);
    }

    PageNo leftmost_child = node.GetLeftmostChild();
    std::vector<InternalEntry> entries = node.ReadInternalEntries();
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
    node = BTreeNodePage(page, key_type_, max_varchar_len_);
    leftmost_child = node.GetLeftmostChild();
    entries = node.ReadInternalEntries();
    old_right = node.GetRightLink();
    had_old_high_key = node.HasHighKey();
    old_high_key.clear();
    if (had_old_high_key) old_high_key = node.GetHighKey();
    child_pos = FindChildPosition(entries, leftmost_child, child_page_no);

    if (!child_result.deleted) {
        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        return result;
    }
    if (child_pos < 0) {
        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        throw std::runtime_error("Child disappeared from parent during delete");
    }

    bool parent_changed = false;
    if (child_result.subtree_first_key_changed && child_pos > 0) {
        entries[static_cast<std::size_t>(child_pos - 1)].key = child_result.new_subtree_first_key;
        parent_changed = true;
    }

    if (child_result.underflow) {
        std::optional<PageId> retired_page_id;
        PageId child_pid{index_relation_id_, child_page_no};
        Page *child_page = buffer_pool_manager_->FetchPage(child_pid);
        if (child_page == nullptr) {
            page->WUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            throw std::runtime_error("Failed to fetch underflowed child page");
        }
        child_page->WLatch();
        BTreeNodePage child_node(child_page, key_type_, max_varchar_len_);

        PageNo left_sibling_page_no = INVALID_PAGE_NO;
        PageNo right_sibling_page_no = INVALID_PAGE_NO;
        if (child_pos > 0) {
            left_sibling_page_no = ChildAt(entries, leftmost_child, static_cast<std::size_t>(child_pos - 1));
        }
        if (static_cast<std::size_t>(child_pos + 1) < entries.size() + 1) {
            right_sibling_page_no = ChildAt(entries, leftmost_child, static_cast<std::size_t>(child_pos + 1));
        }

        if (child_node.IsLeaf()) {
            std::vector<LeafEntry> child_entries = child_node.ReadLeafEntries();
            PageNo child_right = child_node.GetRightLink();
            std::vector<char> child_high_key;
            bool child_has_high_key = child_node.HasHighKey();
            if (child_has_high_key) child_high_key = child_node.GetHighKey();
            bool fixed = false;

            if (left_sibling_page_no != INVALID_PAGE_NO && !fixed) {
                PageId left_pid{index_relation_id_, left_sibling_page_no};
                Page *left_page = buffer_pool_manager_->FetchPage(left_pid);
                if (left_page == nullptr) throw std::runtime_error("Failed to fetch left leaf sibling");
                left_page->WLatch();
                BTreeNodePage left_node(left_page, key_type_, max_varchar_len_);
                std::vector<LeafEntry> left_entries = left_node.ReadLeafEntries();
                if (!left_entries.empty()) {
                    LeafEntry borrowed = left_entries.back();
                    std::vector<LeafEntry> left_after = left_entries;
                    left_after.pop_back();
                    if (!IsLeafUnderfull(left_after)) {
                        std::vector<LeafEntry> child_after = child_entries;
                        child_after.insert(child_after.begin(), borrowed);
                        std::vector<char> new_separator = child_after.front().key;

                        PageNo left_right = left_node.GetRightLink();
                        std::vector<char> before_left(left_page->GetData(), left_page->GetData() + PAGE_SIZE);
                        std::vector<char> before_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                        bool ok_left = left_node.RewriteLeaf(left_after, left_right, &new_separator);
                        bool ok_child = child_node.RewriteLeaf(child_after, child_right,
                                                               child_has_high_key ? &child_high_key : nullptr);
                        if (!ok_left || !ok_child) throw std::runtime_error("Leaf borrow-from-left rewrite failed");
                        std::vector<char> after_left(left_page->GetData(), left_page->GetData() + PAGE_SIZE);
                        std::vector<char> after_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                        LogPageChange(txn, LogRecordType::BTREE_REBALANCE, left_page, left_pid, before_left, after_left);
                        LogPageChange(txn, LogRecordType::BTREE_REBALANCE, child_page, child_pid, before_child, after_child);
                        entries[static_cast<std::size_t>(child_pos - 1)].key = new_separator;
                        parent_changed = true;
                        fixed = true;
                    }
                }
                left_page->WUnlatch();
                buffer_pool_manager_->UnpinPage(left_pid, fixed);
            }

            if (right_sibling_page_no != INVALID_PAGE_NO && !fixed) {
                PageId right_pid{index_relation_id_, right_sibling_page_no};
                Page *right_page = buffer_pool_manager_->FetchPage(right_pid);
                if (right_page == nullptr) throw std::runtime_error("Failed to fetch right leaf sibling");
                right_page->WLatch();
                BTreeNodePage right_node(right_page, key_type_, max_varchar_len_);
                std::vector<LeafEntry> right_entries = right_node.ReadLeafEntries();
                if (!right_entries.empty()) {
                    LeafEntry borrowed = right_entries.front();
                    std::vector<LeafEntry> right_after(right_entries.begin() + 1, right_entries.end());
                    if (!right_after.empty() && !IsLeafUnderfull(right_after)) {
                        std::vector<LeafEntry> child_after = child_entries;
                        child_after.push_back(borrowed);
                        std::vector<char> new_separator = right_after.front().key;
                        PageNo right_right = right_node.GetRightLink();
                        std::vector<char> right_high_key;
                        bool right_has_high_key = right_node.HasHighKey();
                        if (right_has_high_key) right_high_key = right_node.GetHighKey();

                        std::vector<char> before_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                        std::vector<char> before_right(right_page->GetData(), right_page->GetData() + PAGE_SIZE);
                        bool ok_child = child_node.RewriteLeaf(child_after, child_right, &new_separator);
                        bool ok_right = right_node.RewriteLeaf(right_after, right_right,
                                                               right_has_high_key ? &right_high_key : nullptr);
                        if (!ok_child || !ok_right) throw std::runtime_error("Leaf borrow-from-right rewrite failed");
                        std::vector<char> after_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                        std::vector<char> after_right(right_page->GetData(), right_page->GetData() + PAGE_SIZE);
                        LogPageChange(txn, LogRecordType::BTREE_REBALANCE, child_page, child_pid, before_child, after_child);
                        LogPageChange(txn, LogRecordType::BTREE_REBALANCE, right_page, right_pid, before_right, after_right);
                        entries[static_cast<std::size_t>(child_pos)].key = new_separator;
                        parent_changed = true;
                        fixed = true;
                    }
                }
                right_page->WUnlatch();
                buffer_pool_manager_->UnpinPage(right_pid, fixed);
            }

            if (!fixed && left_sibling_page_no != INVALID_PAGE_NO) {
                PageId left_pid{index_relation_id_, left_sibling_page_no};
                Page *left_page = buffer_pool_manager_->FetchPage(left_pid);
                if (left_page == nullptr) throw std::runtime_error("Failed to fetch left leaf sibling for merge");
                left_page->WLatch();
                BTreeNodePage left_node(left_page, key_type_, max_varchar_len_);
                std::vector<LeafEntry> left_entries = left_node.ReadLeafEntries();
                std::vector<LeafEntry> merged = left_entries;
                merged.insert(merged.end(), child_entries.begin(), child_entries.end());
                if (!CanFitLeafEntries(merged)) {
                    left_page->WUnlatch();
                    buffer_pool_manager_->UnpinPage(left_pid, false);
                    child_page->WUnlatch();
                    buffer_pool_manager_->UnpinPage(child_pid, false);
                    page->WUnlatch();
                    buffer_pool_manager_->UnpinPage(pid, false);
                    throw std::runtime_error("Merged leaf does not fit in one page");
                }
                std::vector<char> before_left(left_page->GetData(), left_page->GetData() + PAGE_SIZE);
                bool ok_left = left_node.RewriteLeaf(merged, child_right, child_has_high_key ? &child_high_key : nullptr);
                if (!ok_left) throw std::runtime_error("Leaf merge-into-left rewrite failed");
                std::vector<char> after_left(left_page->GetData(), left_page->GetData() + PAGE_SIZE);
                LogPageChange(txn, LogRecordType::BTREE_MERGE, left_page, left_pid, before_left, after_left);
                entries.erase(entries.begin() + (child_pos - 1));
                retired_page_id = child_pid;
                parent_changed = true;
                fixed = true;
                left_page->WUnlatch();
                buffer_pool_manager_->UnpinPage(left_pid, true);
            }

            if (!fixed && right_sibling_page_no != INVALID_PAGE_NO) {
                PageId right_pid{index_relation_id_, right_sibling_page_no};
                Page *right_page = buffer_pool_manager_->FetchPage(right_pid);
                if (right_page == nullptr) throw std::runtime_error("Failed to fetch right leaf sibling for merge");
                right_page->WLatch();
                BTreeNodePage right_node(right_page, key_type_, max_varchar_len_);
                std::vector<LeafEntry> right_entries = right_node.ReadLeafEntries();
                std::vector<LeafEntry> merged = child_entries;
                merged.insert(merged.end(), right_entries.begin(), right_entries.end());
                if (!CanFitLeafEntries(merged)) {
                    right_page->WUnlatch();
                    buffer_pool_manager_->UnpinPage(right_pid, false);
                    child_page->WUnlatch();
                    buffer_pool_manager_->UnpinPage(child_pid, false);
                    page->WUnlatch();
                    buffer_pool_manager_->UnpinPage(pid, false);
                    throw std::runtime_error("Merged leaf does not fit in one page");
                }
                PageNo right_right = right_node.GetRightLink();
                std::vector<char> right_high_key;
                bool right_has_high_key = right_node.HasHighKey();
                if (right_has_high_key) right_high_key = right_node.GetHighKey();
                std::vector<char> before_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                bool ok_child = child_node.RewriteLeaf(merged, right_right,
                                                       right_has_high_key ? &right_high_key : nullptr);
                if (!ok_child) throw std::runtime_error("Leaf merge-with-right rewrite failed");
                std::vector<char> after_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                LogPageChange(txn, LogRecordType::BTREE_MERGE, child_page, child_pid, before_child, after_child);
                entries.erase(entries.begin() + child_pos);
                retired_page_id = right_pid;
                parent_changed = true;
                fixed = true;
                right_page->WUnlatch();
                buffer_pool_manager_->UnpinPage(right_pid, false);
            }

            if (!fixed) {
                child_page->WUnlatch();
                buffer_pool_manager_->UnpinPage(child_pid, false);
                page->WUnlatch();
                buffer_pool_manager_->UnpinPage(pid, false);
                throw std::runtime_error("Failed to rebalance underflowed leaf");
            }
        } else {
            std::vector<InternalEntry> child_entries = child_node.ReadInternalEntries();
            PageNo child_leftmost = child_node.GetLeftmostChild();
            PageNo child_right = child_node.GetRightLink();
            std::vector<char> child_high_key;
            bool child_has_high_key = child_node.HasHighKey();
            if (child_has_high_key) child_high_key = child_node.GetHighKey();
            bool fixed = false;

            if (left_sibling_page_no != INVALID_PAGE_NO && !fixed) {
                PageId left_pid{index_relation_id_, left_sibling_page_no};
                Page *left_page = buffer_pool_manager_->FetchPage(left_pid);
                if (left_page == nullptr) throw std::runtime_error("Failed to fetch left internal sibling");
                left_page->WLatch();
                BTreeNodePage left_node(left_page, key_type_, max_varchar_len_);
                std::vector<InternalEntry> left_entries = left_node.ReadInternalEntries();
                if (!left_entries.empty()) {
                    InternalEntry borrowed = left_entries.back();
                    std::vector<InternalEntry> left_after = left_entries;
                    left_after.pop_back();
                    if (!IsInternalUnderfull(left_after)) {
                        std::vector<InternalEntry> child_after = child_entries;
                        child_after.insert(child_after.begin(), InternalEntry{entries[static_cast<std::size_t>(child_pos - 1)].key, child_leftmost});
                        PageNo new_child_leftmost = borrowed.child;
                        std::vector<char> new_separator = borrowed.key;

                        PageNo left_right = left_node.GetRightLink();
                        std::vector<char> before_left(left_page->GetData(), left_page->GetData() + PAGE_SIZE);
                        std::vector<char> before_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                        bool ok_left = left_node.RewriteInternal(left_node.GetLeftmostChild(), left_after, left_right, &new_separator);
                        bool ok_child = child_node.RewriteInternal(new_child_leftmost, child_after, child_right,
                                                                   child_has_high_key ? &child_high_key : nullptr);
                        if (!ok_left || !ok_child) throw std::runtime_error("Internal borrow-from-left rewrite failed");
                        std::vector<char> after_left(left_page->GetData(), left_page->GetData() + PAGE_SIZE);
                        std::vector<char> after_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                        LogPageChange(txn, LogRecordType::BTREE_REBALANCE, left_page, left_pid, before_left, after_left);
                        LogPageChange(txn, LogRecordType::BTREE_REBALANCE, child_page, child_pid, before_child, after_child);
                        entries[static_cast<std::size_t>(child_pos - 1)].key = new_separator;
                        parent_changed = true;
                        fixed = true;
                    }
                }
                left_page->WUnlatch();
                buffer_pool_manager_->UnpinPage(left_pid, fixed);
            }

            if (right_sibling_page_no != INVALID_PAGE_NO && !fixed) {
                PageId right_pid{index_relation_id_, right_sibling_page_no};
                Page *right_page = buffer_pool_manager_->FetchPage(right_pid);
                if (right_page == nullptr) throw std::runtime_error("Failed to fetch right internal sibling");
                right_page->WLatch();
                BTreeNodePage right_node(right_page, key_type_, max_varchar_len_);
                std::vector<InternalEntry> right_entries = right_node.ReadInternalEntries();
                if (!right_entries.empty()) {
                    std::vector<InternalEntry> right_after(right_entries.begin() + 1, right_entries.end());
                    if (!IsInternalUnderfull(right_after)) {
                        PageNo borrowed_leftmost_child = right_node.GetLeftmostChild();
                        std::vector<InternalEntry> child_after = child_entries;
                        child_after.push_back(InternalEntry{entries[static_cast<std::size_t>(child_pos)].key, borrowed_leftmost_child});
                        std::vector<char> new_separator = right_entries.front().key;
                        PageNo new_right_leftmost = right_entries.front().child;

                        PageNo right_right = right_node.GetRightLink();
                        std::vector<char> right_high_key;
                        bool right_has_high_key = right_node.HasHighKey();
                        if (right_has_high_key) right_high_key = right_node.GetHighKey();

                        std::vector<char> before_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                        std::vector<char> before_right(right_page->GetData(), right_page->GetData() + PAGE_SIZE);
                        bool ok_child = child_node.RewriteInternal(child_leftmost, child_after, child_right, &new_separator);
                        bool ok_right = right_node.RewriteInternal(new_right_leftmost, right_after, right_right,
                                                                   right_has_high_key ? &right_high_key : nullptr);
                        if (!ok_child || !ok_right) throw std::runtime_error("Internal borrow-from-right rewrite failed");
                        std::vector<char> after_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                        std::vector<char> after_right(right_page->GetData(), right_page->GetData() + PAGE_SIZE);
                        LogPageChange(txn, LogRecordType::BTREE_REBALANCE, child_page, child_pid, before_child, after_child);
                        LogPageChange(txn, LogRecordType::BTREE_REBALANCE, right_page, right_pid, before_right, after_right);
                        entries[static_cast<std::size_t>(child_pos)].key = new_separator;
                        parent_changed = true;
                        fixed = true;
                    }
                }
                right_page->WUnlatch();
                buffer_pool_manager_->UnpinPage(right_pid, fixed);
            }

            if (!fixed && left_sibling_page_no != INVALID_PAGE_NO) {
                PageId left_pid{index_relation_id_, left_sibling_page_no};
                Page *left_page = buffer_pool_manager_->FetchPage(left_pid);
                if (left_page == nullptr) throw std::runtime_error("Failed to fetch left internal sibling for merge");
                left_page->WLatch();
                BTreeNodePage left_node(left_page, key_type_, max_varchar_len_);
                std::vector<InternalEntry> left_entries = left_node.ReadInternalEntries();
                std::vector<InternalEntry> merged = left_entries;
                merged.push_back(InternalEntry{entries[static_cast<std::size_t>(child_pos - 1)].key, child_leftmost});
                merged.insert(merged.end(), child_entries.begin(), child_entries.end());
                if (!CanFitInternalEntries(left_node.GetLeftmostChild(), merged)) {
                    left_page->WUnlatch();
                    buffer_pool_manager_->UnpinPage(left_pid, false);
                    child_page->WUnlatch();
                    buffer_pool_manager_->UnpinPage(child_pid, false);
                    page->WUnlatch();
                    buffer_pool_manager_->UnpinPage(pid, false);
                    throw std::runtime_error("Merged internal node does not fit in one page");
                }
                std::vector<char> before_left(left_page->GetData(), left_page->GetData() + PAGE_SIZE);
                bool ok_left = left_node.RewriteInternal(left_node.GetLeftmostChild(), merged, child_right,
                                                         child_has_high_key ? &child_high_key : nullptr);
                if (!ok_left) throw std::runtime_error("Internal merge-into-left rewrite failed");
                std::vector<char> after_left(left_page->GetData(), left_page->GetData() + PAGE_SIZE);
                LogPageChange(txn, LogRecordType::BTREE_MERGE, left_page, left_pid, before_left, after_left);
                entries.erase(entries.begin() + (child_pos - 1));
                retired_page_id = child_pid;
                parent_changed = true;
                fixed = true;
                left_page->WUnlatch();
                buffer_pool_manager_->UnpinPage(left_pid, true);
            }

            if (!fixed && right_sibling_page_no != INVALID_PAGE_NO) {
                PageId right_pid{index_relation_id_, right_sibling_page_no};
                Page *right_page = buffer_pool_manager_->FetchPage(right_pid);
                if (right_page == nullptr) throw std::runtime_error("Failed to fetch right internal sibling for merge");
                right_page->WLatch();
                BTreeNodePage right_node(right_page, key_type_, max_varchar_len_);
                std::vector<InternalEntry> right_entries = right_node.ReadInternalEntries();
                std::vector<InternalEntry> merged = child_entries;
                merged.push_back(InternalEntry{entries[static_cast<std::size_t>(child_pos)].key, right_node.GetLeftmostChild()});
                merged.insert(merged.end(), right_entries.begin(), right_entries.end());
                if (!CanFitInternalEntries(child_leftmost, merged)) {
                    right_page->WUnlatch();
                    buffer_pool_manager_->UnpinPage(right_pid, false);
                    child_page->WUnlatch();
                    buffer_pool_manager_->UnpinPage(child_pid, false);
                    page->WUnlatch();
                    buffer_pool_manager_->UnpinPage(pid, false);
                    throw std::runtime_error("Merged internal node does not fit in one page");
                }
                PageNo right_right = right_node.GetRightLink();
                std::vector<char> right_high_key;
                bool right_has_high_key = right_node.HasHighKey();
                if (right_has_high_key) right_high_key = right_node.GetHighKey();
                std::vector<char> before_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                bool ok_child = child_node.RewriteInternal(child_leftmost, merged, right_right,
                                                           right_has_high_key ? &right_high_key : nullptr);
                if (!ok_child) throw std::runtime_error("Internal merge-with-right rewrite failed");
                std::vector<char> after_child(child_page->GetData(), child_page->GetData() + PAGE_SIZE);
                LogPageChange(txn, LogRecordType::BTREE_MERGE, child_page, child_pid, before_child, after_child);
                entries.erase(entries.begin() + child_pos);
                retired_page_id = right_pid;
                parent_changed = true;
                fixed = true;
                right_page->WUnlatch();
                buffer_pool_manager_->UnpinPage(right_pid, false);
            }

            if (!fixed) {
                child_page->WUnlatch();
                buffer_pool_manager_->UnpinPage(child_pid, false);
                page->WUnlatch();
                buffer_pool_manager_->UnpinPage(pid, false);
                throw std::runtime_error("Failed to rebalance underflowed internal node");
            }
        }

        child_page->WUnlatch();
        buffer_pool_manager_->UnpinPage(child_pid, false);
        if (retired_page_id.has_value()) {
            buffer_pool_manager_->GetPageRetireManager()->RetirePage(
                *retired_page_id,
                KeepRetiredPageForRecovery);
        }
    }

    result.deleted = true;

    if (is_root) {
        if (entries.empty()) {
            PageNo new_root = leftmost_child;
            BTreeMetaPageData meta = ReadMetaPage();
            if (meta.root_page_no == page_no && meta.tree_height > 1) {
                meta.root_page_no = new_root;
                meta.tree_height--;
                WriteMetaPage(meta, txn);
            }
            page->WUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            buffer_pool_manager_->GetPageRetireManager()->RetirePage(
                pid,
                KeepRetiredPageForRecovery);
            return result;
        }
    }

    std::vector<char> before(page->GetData(), page->GetData() + PAGE_SIZE);
    if (!node.RewriteInternal(leftmost_child, entries, old_right, had_old_high_key ? &old_high_key : nullptr)) {
        page->WUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        throw std::runtime_error("Failed to rewrite internal page during delete");
    }
    std::vector<char> after(page->GetData(), page->GetData() + PAGE_SIZE);
    if (parent_changed || child_result.underflow) {
        LogPageChange(txn, LogRecordType::BTREE_REBALANCE, page, pid, before, after);
    }

    result.underflow = !is_root && IsInternalUnderfull(entries);
    if (!is_root) {
        std::optional<std::vector<char>> new_first_key = GetSubtreeFirstKey(page_no);
        if (old_subtree_first_key.has_value() != new_first_key.has_value() ||
            (old_subtree_first_key.has_value() &&
             IndexKeyUtil::CompareEncoded(key_type_, *old_subtree_first_key, *new_first_key) != 0)) {
            result.subtree_first_key_changed = new_first_key.has_value();
            if (new_first_key.has_value()) result.new_subtree_first_key = *new_first_key;
        }
    }

    page->WUnlatch();
    buffer_pool_manager_->UnpinPage(pid, true);
    return result;
}

}  // namespace simpledb
