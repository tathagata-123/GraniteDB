// Public B+ tree delete entry points and delete descent protocol.

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

bool BTreeIndex::Delete(const Value &key, const RID &rid) {
    return Delete(nullptr, key, rid);
}

bool BTreeIndex::Delete(const TransactionPtr &txn, const Value &key, const RID &rid) {
    auto op = buffer_pool_manager_->GetPageRetireManager()->Guard();
    std::vector<char> encoded_key = IndexKeyUtil::EncodeValue(key, key_type_, max_varchar_len_);
    BTreeMetaPageData meta = ReadMetaPage();
    PageId root_pid{index_relation_id_, meta.root_page_no};
    Page *current_page = buffer_pool_manager_->FetchPage(root_pid);
    if (current_page == nullptr) throw std::runtime_error("Failed to fetch B+ tree root page during delete");
    current_page->WLatch();
    PageId current_pid = root_pid;
    std::vector<WriteLatchFrame> path;

    auto move_right = [&](PageId &pid, Page *&page) {
        BTreeNodePage node(page, key_type_, max_varchar_len_);
        while (node.HasHighKey() && node.GetRightLink() != INVALID_PAGE_NO &&
               IndexKeyUtil::CompareEncoded(key_type_, encoded_key, node.GetHighKey()) >= 0) {
            PageId right_pid{index_relation_id_, node.GetRightLink()};
            Page *right_page = buffer_pool_manager_->FetchPage(right_pid);
            if (right_page == nullptr) {
                ReleaseWritePath(buffer_pool_manager_, path);
                ReleaseWriteFrame(buffer_pool_manager_, {pid, page}, false);
                throw std::runtime_error("Failed to move right during B+ tree delete descent");
            }
            right_page->WLatch();
            ReleaseWriteFrame(buffer_pool_manager_, {pid, page}, false);
            pid = right_pid;
            page = right_page;
            node = BTreeNodePage(page, key_type_, max_varchar_len_);
        }
    };

    auto child_safe_for_delete = [&](Page *page) {
        BTreeNodePage node(page, key_type_, max_varchar_len_);
        if (node.IsLeaf()) {
            std::vector<LeafEntry> entries = node.ReadLeafEntries();
            uint32_t used = LeafUsedBytes(entries);
            uint32_t capacity = NodeCapacityBytes(node.GetSpecialSize());
            uint32_t drop = std::min(used, MaxLeafInsertBytes(max_key_bytes_));
            return used >= drop && used - drop >= capacity / 2;
        }
        std::vector<InternalEntry> entries = node.ReadInternalEntries();
        uint32_t used = InternalUsedBytes(entries);
        uint32_t capacity = NodeCapacityBytes(node.GetSpecialSize());
        uint32_t drop = std::min(used, MaxInternalInsertBytes(max_key_bytes_));
        return used >= drop && used - drop >= capacity / 2;
    };

    while (true) {
        move_right(current_pid, current_page);
        BTreeNodePage current(current_page, key_type_, max_varchar_len_);
        if (current.IsLeaf()) break;

        PageId child_pid{index_relation_id_, current.FindChildForKey(encoded_key)};
        Page *child_page = buffer_pool_manager_->FetchPage(child_pid);
        if (child_page == nullptr) {
            ReleaseWritePath(buffer_pool_manager_, path);
            ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, false);
            throw std::runtime_error("Failed to fetch child page during B+ tree delete descent");
        }
        child_page->WLatch();
        move_right(child_pid, child_page);

        if (child_safe_for_delete(child_page)) {
            ReleaseWritePath(buffer_pool_manager_, path);
            ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, false);
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
            throw std::runtime_error("Failed to rewrite B+ tree leaf page during delete");
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
            throw std::runtime_error("Failed to rewrite B+ tree internal page during delete");
        }
        std::vector<char> after(page->GetData(), page->GetData() + PAGE_SIZE);
        LogPageChange(txn, type, page, pid, before, after);
    };

    BTreeNodePage leaf_node(current_page, key_type_, max_varchar_len_);
    std::vector<LeafEntry> leaf_entries = leaf_node.ReadLeafEntries();
    auto it = std::find_if(leaf_entries.begin(), leaf_entries.end(), [&](const LeafEntry &entry) {
        return IndexKeyUtil::CompareEncoded(key_type_, entry.key, encoded_key) == 0 && entry.rid == rid;
    });
    if (it == leaf_entries.end()) {
        ReleaseWritePath(buffer_pool_manager_, path);
        ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, false);
        return false;
    }
    leaf_entries.erase(it);

    PageNo child_right = leaf_node.GetRightLink();
    std::vector<char> child_high_key;
    bool child_has_high_key = leaf_node.HasHighKey();
    if (child_has_high_key) child_high_key = leaf_node.GetHighKey();

    rewrite_leaf(current_pid, current_page, leaf_entries, child_right, child_has_high_key ? &child_high_key : nullptr,
                 LogRecordType::BTREE_DELETE);
    if (current_pid.page_no == root_pid.page_no || !IsLeafUnderfull(leaf_entries)) {
        ReleaseWritePath(buffer_pool_manager_, path);
        ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, true);
        return true;
    }

    auto schedule_retire = [&](PageId pid) {
        buffer_pool_manager_->GetPageRetireManager()->RetirePage(
            pid,
            KeepRetiredPageForRecovery);
    };

    auto retire_leaf_page = [&](PageId pid, Page *page, PageNo redirect_page_no, PageNo right_link,
                                const std::vector<char> *high_key_or_null) {
        std::vector<char> before(page->GetData(), page->GetData() + PAGE_SIZE);
        BTreeNodePage node(page, key_type_, max_varchar_len_);
        assert(!node.IsRetired());
        if (!node.RewriteLeaf({}, right_link, high_key_or_null)) {
            throw std::runtime_error("Failed to rewrite retired B+ tree leaf page");
        }
        node.SetLeftmostChild(redirect_page_no);
        node.SetPageState(BTreePageState::RETIRED);
        std::vector<char> after(page->GetData(), page->GetData() + PAGE_SIZE);
        LogPageChange(txn, LogRecordType::BTREE_MERGE, page, pid, before, after);
    };

    auto retire_internal_page = [&](PageId pid, Page *page, PageNo redirect_child, PageNo right_link,
                                    const std::vector<char> *high_key_or_null) {
        std::vector<char> before(page->GetData(), page->GetData() + PAGE_SIZE);
        BTreeNodePage node(page, key_type_, max_varchar_len_);
        assert(!node.IsRetired());
        if (!node.RewriteInternal(redirect_child, {}, right_link, high_key_or_null)) {
            throw std::runtime_error("Failed to rewrite retired B+ tree internal page");
        }
        node.SetPageState(BTreePageState::RETIRED);
        std::vector<char> after(page->GetData(), page->GetData() + PAGE_SIZE);
        LogPageChange(txn, LogRecordType::BTREE_MERGE, page, pid, before, after);
    };

    while (true) {
        if (path.empty()) {
            ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, true);
            return true;
        }

        WriteLatchFrame parent_frame = path.back();
        path.pop_back();
        PageId parent_pid = parent_frame.pid;
        Page *parent_page = parent_frame.page;
        BTreeNodePage parent_node(parent_page, key_type_, max_varchar_len_);
        PageNo parent_leftmost = parent_node.GetLeftmostChild();
        std::vector<InternalEntry> parent_entries = parent_node.ReadInternalEntries();
        PageNo parent_right = parent_node.GetRightLink();
        std::vector<char> parent_high_key;
        bool parent_has_high_key = parent_node.HasHighKey();
        if (parent_has_high_key) parent_high_key = parent_node.GetHighKey();

        int child_pos = FindChildPosition(parent_entries, parent_leftmost, current_pid.page_no);
        if (child_pos < 0) {
            ReleaseWritePath(buffer_pool_manager_, path);
            ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, false);
            ReleaseWriteFrame(buffer_pool_manager_, parent_frame, false);
            throw std::runtime_error("Parent did not reference target child during B+ tree delete fixup");
        }

        bool child_is_leaf = BTreeNodePage(current_page, key_type_, max_varchar_len_).IsLeaf();
        PageNo left_sibling_page_no = INVALID_PAGE_NO;
        PageNo right_sibling_page_no = INVALID_PAGE_NO;
        if (child_pos > 0) left_sibling_page_no = ChildAt(parent_entries, parent_leftmost, static_cast<std::size_t>(child_pos - 1));
        if (static_cast<std::size_t>(child_pos + 1) < parent_entries.size() + 1) {
            right_sibling_page_no = ChildAt(parent_entries, parent_leftmost, static_cast<std::size_t>(child_pos + 1));
        }

        bool fixed = false;
        std::optional<PageId> retired_page_id;
        bool child_dirty = true;

        auto relatch_pair = [&](PageId first_pid, Page *&first_page, PageId second_pid, Page *&second_page) {
            if (first_page == nullptr) {
                first_page = buffer_pool_manager_->FetchPage(first_pid);
                if (first_page == nullptr) throw std::runtime_error("Failed to fetch sibling page during delete fixup");
            }
            if (second_page == nullptr) {
                second_page = buffer_pool_manager_->FetchPage(second_pid);
                if (second_page == nullptr) {
                    buffer_pool_manager_->UnpinPage(first_pid, false);
                    throw std::runtime_error("Failed to fetch child page during ordered relatch");
                }
            }
            assert(first_pid.page_no != second_pid.page_no);
            if (first_pid.page_no < second_pid.page_no) {
                first_page->WLatch();
                second_page->WLatch();
            } else {
                second_page->WLatch();
                first_page->WLatch();
            }
        };

        if (child_is_leaf) {
            if (left_sibling_page_no != INVALID_PAGE_NO && !fixed) {
                PageId left_pid{index_relation_id_, left_sibling_page_no};
                Page *left_page = buffer_pool_manager_->FetchPage(left_pid);
                current_page->WUnlatch();
                relatch_pair(left_pid, left_page, current_pid, current_page);
                BTreeNodePage left_node(left_page, key_type_, max_varchar_len_);
                BTreeNodePage child_node2(current_page, key_type_, max_varchar_len_);
                std::vector<LeafEntry> left_entries = left_node.ReadLeafEntries();
                std::vector<LeafEntry> child_entries = child_node2.ReadLeafEntries();
                if (!left_entries.empty()) {
                    std::vector<LeafEntry> left_after = left_entries;
                    LeafEntry borrowed = left_after.back();
                    left_after.pop_back();
                    if (!IsLeafUnderfull(left_after)) {
                        std::vector<LeafEntry> child_after = child_entries;
                        child_after.insert(child_after.begin(), borrowed);
                        std::vector<char> new_separator = child_after.front().key;
                        std::vector<char> left_high_key;
                        bool left_has_high_key = left_node.HasHighKey();
                        if (left_has_high_key) left_high_key = left_node.GetHighKey();
                        rewrite_leaf(left_pid, left_page, left_after, current_pid.page_no, &new_separator, LogRecordType::BTREE_REBALANCE);
                        std::vector<char> child_high_key2;
                        bool child_has_high_key2 = child_node2.HasHighKey();
                        if (child_has_high_key2) child_high_key2 = child_node2.GetHighKey();
                        rewrite_leaf(current_pid, current_page, child_after, child_node2.GetRightLink(),
                                     child_has_high_key2 ? &child_high_key2 : nullptr,
                                     LogRecordType::BTREE_REBALANCE);
                        parent_entries[static_cast<std::size_t>(child_pos - 1)].key = new_separator;
                        fixed = true;
                    }
                }
                left_page->WUnlatch();
                buffer_pool_manager_->UnpinPage(left_pid, fixed);
            }

            if (right_sibling_page_no != INVALID_PAGE_NO && !fixed) {
                PageId right_pid{index_relation_id_, right_sibling_page_no};
                Page *right_page = buffer_pool_manager_->FetchPage(right_pid);
                right_page->WLatch();
                BTreeNodePage right_node(right_page, key_type_, max_varchar_len_);
                BTreeNodePage child_node2(current_page, key_type_, max_varchar_len_);
                std::vector<LeafEntry> right_entries = right_node.ReadLeafEntries();
                std::vector<LeafEntry> child_entries = child_node2.ReadLeafEntries();
                if (!right_entries.empty()) {
                    std::vector<LeafEntry> right_after(right_entries.begin() + 1, right_entries.end());
                    if (!right_after.empty() && !IsLeafUnderfull(right_after)) {
                        LeafEntry borrowed = right_entries.front();
                        std::vector<LeafEntry> child_after = child_entries;
                        child_after.push_back(borrowed);
                        std::vector<char> new_separator = right_after.front().key;
                        std::vector<char> right_high_key;
                        bool right_has_high_key = right_node.HasHighKey();
                        if (right_has_high_key) right_high_key = right_node.GetHighKey();
                        rewrite_leaf(current_pid, current_page, child_after, child_node2.GetRightLink(), &new_separator,
                                     LogRecordType::BTREE_REBALANCE);
                        rewrite_leaf(right_pid, right_page, right_after, right_node.GetRightLink(),
                                     right_has_high_key ? &right_high_key : nullptr, LogRecordType::BTREE_REBALANCE);
                        parent_entries[static_cast<std::size_t>(child_pos)].key = new_separator;
                        fixed = true;
                    }
                }
                right_page->WUnlatch();
                buffer_pool_manager_->UnpinPage(right_pid, fixed);
            }

            if (!fixed && left_sibling_page_no != INVALID_PAGE_NO) {
                PageId left_pid{index_relation_id_, left_sibling_page_no};
                Page *left_page = buffer_pool_manager_->FetchPage(left_pid);
                current_page->WUnlatch();
                relatch_pair(left_pid, left_page, current_pid, current_page);
                BTreeNodePage left_node(left_page, key_type_, max_varchar_len_);
                BTreeNodePage child_node2(current_page, key_type_, max_varchar_len_);
                std::vector<LeafEntry> merged = left_node.ReadLeafEntries();
                std::vector<LeafEntry> child_entries = child_node2.ReadLeafEntries();
                merged.insert(merged.end(), child_entries.begin(), child_entries.end());
                if (!CanFitLeafEntries(merged)) {
                    left_page->WUnlatch();
                    buffer_pool_manager_->UnpinPage(left_pid, false);
                    ReleaseWritePath(buffer_pool_manager_, path);
                    ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, false);
                    ReleaseWriteFrame(buffer_pool_manager_, parent_frame, false);
                    throw std::runtime_error("Merged leaf node does not fit in one page");
                }
                std::vector<char> child_high_key2;
                bool child_has_high_key2 = child_node2.HasHighKey();
                if (child_has_high_key2) child_high_key2 = child_node2.GetHighKey();
                rewrite_leaf(left_pid, left_page, merged, child_node2.GetRightLink(),
                             child_has_high_key2 ? &child_high_key2 : nullptr, LogRecordType::BTREE_MERGE);
                retire_leaf_page(current_pid, current_page, left_pid.page_no, child_node2.GetRightLink(),
                                 child_has_high_key2 ? &child_high_key2 : nullptr);
                parent_entries.erase(parent_entries.begin() + (child_pos - 1));
                retired_page_id = current_pid;
                fixed = true;
                left_page->WUnlatch();
                buffer_pool_manager_->UnpinPage(left_pid, true);
            }

            if (!fixed && right_sibling_page_no != INVALID_PAGE_NO) {
                PageId right_pid{index_relation_id_, right_sibling_page_no};
                Page *right_page = buffer_pool_manager_->FetchPage(right_pid);
                right_page->WLatch();
                BTreeNodePage right_node(right_page, key_type_, max_varchar_len_);
                BTreeNodePage child_node2(current_page, key_type_, max_varchar_len_);
                std::vector<LeafEntry> merged = child_node2.ReadLeafEntries();
                std::vector<LeafEntry> right_entries = right_node.ReadLeafEntries();
                merged.insert(merged.end(), right_entries.begin(), right_entries.end());
                if (!CanFitLeafEntries(merged)) {
                    right_page->WUnlatch();
                    buffer_pool_manager_->UnpinPage(right_pid, false);
                    ReleaseWritePath(buffer_pool_manager_, path);
                    ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, false);
                    ReleaseWriteFrame(buffer_pool_manager_, parent_frame, false);
                    throw std::runtime_error("Merged leaf node does not fit in one page");
                }
                std::vector<char> right_high_key;
                bool right_has_high_key = right_node.HasHighKey();
                if (right_has_high_key) right_high_key = right_node.GetHighKey();
                rewrite_leaf(current_pid, current_page, merged, right_node.GetRightLink(),
                             right_has_high_key ? &right_high_key : nullptr, LogRecordType::BTREE_MERGE);
                retire_leaf_page(right_pid, right_page, current_pid.page_no, right_node.GetRightLink(),
                                 right_has_high_key ? &right_high_key : nullptr);
                parent_entries.erase(parent_entries.begin() + child_pos);
                retired_page_id = right_pid;
                fixed = true;
                right_page->WUnlatch();
                buffer_pool_manager_->UnpinPage(right_pid, true);
            }
        } else {
            if (left_sibling_page_no != INVALID_PAGE_NO && !fixed) {
                PageId left_pid{index_relation_id_, left_sibling_page_no};
                Page *left_page = buffer_pool_manager_->FetchPage(left_pid);
                current_page->WUnlatch();
                relatch_pair(left_pid, left_page, current_pid, current_page);
                BTreeNodePage left_node(left_page, key_type_, max_varchar_len_);
                BTreeNodePage child_node2(current_page, key_type_, max_varchar_len_);
                std::vector<InternalEntry> left_entries = left_node.ReadInternalEntries();
                std::vector<InternalEntry> child_entries = child_node2.ReadInternalEntries();
                if (!left_entries.empty()) {
                    std::vector<InternalEntry> left_after = left_entries;
                    InternalEntry borrowed = left_after.back();
                    left_after.pop_back();
                    if (!IsInternalUnderfull(left_after)) {
                        std::vector<InternalEntry> child_after = child_entries;
                        child_after.insert(child_after.begin(), InternalEntry{parent_entries[static_cast<std::size_t>(child_pos - 1)].key,
                                                                              child_node2.GetLeftmostChild()});
                        std::vector<char> new_separator = borrowed.key;
                        PageNo child_leftmost = borrowed.child;
                        rewrite_internal(left_pid, left_page, left_node.GetLeftmostChild(), left_after, current_pid.page_no,
                                         &new_separator, LogRecordType::BTREE_REBALANCE);
                        std::vector<char> child_high_key2;
                        bool child_has_high_key2 = child_node2.HasHighKey();
                        if (child_has_high_key2) child_high_key2 = child_node2.GetHighKey();
                        rewrite_internal(current_pid, current_page, child_leftmost, child_after, child_node2.GetRightLink(),
                                         child_has_high_key2 ? &child_high_key2 : nullptr,
                                         LogRecordType::BTREE_REBALANCE);
                        parent_entries[static_cast<std::size_t>(child_pos - 1)].key = new_separator;
                        fixed = true;
                    }
                }
                left_page->WUnlatch();
                buffer_pool_manager_->UnpinPage(left_pid, fixed);
            }

            if (right_sibling_page_no != INVALID_PAGE_NO && !fixed) {
                PageId right_pid{index_relation_id_, right_sibling_page_no};
                Page *right_page = buffer_pool_manager_->FetchPage(right_pid);
                right_page->WLatch();
                BTreeNodePage right_node(right_page, key_type_, max_varchar_len_);
                BTreeNodePage child_node2(current_page, key_type_, max_varchar_len_);
                std::vector<InternalEntry> right_entries = right_node.ReadInternalEntries();
                std::vector<InternalEntry> child_entries = child_node2.ReadInternalEntries();
                if (!right_entries.empty()) {
                    std::vector<InternalEntry> right_after(right_entries.begin() + 1, right_entries.end());
                    if (!right_after.empty() && !IsInternalUnderfull(right_after)) {
                        PageNo borrowed_leftmost = right_node.GetLeftmostChild();
                        std::vector<InternalEntry> child_after = child_entries;
                        child_after.push_back(InternalEntry{parent_entries[static_cast<std::size_t>(child_pos)].key,
                                                            borrowed_leftmost});
                        std::vector<char> new_separator = right_entries.front().key;
                        PageNo new_right_leftmost = right_entries.front().child;
                        std::vector<char> right_high_key;
                        bool right_has_high_key = right_node.HasHighKey();
                        if (right_has_high_key) right_high_key = right_node.GetHighKey();
                        rewrite_internal(current_pid, current_page, child_node2.GetLeftmostChild(), child_after,
                                         child_node2.GetRightLink(), &new_separator, LogRecordType::BTREE_REBALANCE);
                        rewrite_internal(right_pid, right_page, new_right_leftmost, right_after, right_node.GetRightLink(),
                                         right_has_high_key ? &right_high_key : nullptr, LogRecordType::BTREE_REBALANCE);
                        parent_entries[static_cast<std::size_t>(child_pos)].key = new_separator;
                        fixed = true;
                    }
                }
                right_page->WUnlatch();
                buffer_pool_manager_->UnpinPage(right_pid, fixed);
            }

            if (!fixed && left_sibling_page_no != INVALID_PAGE_NO) {
                PageId left_pid{index_relation_id_, left_sibling_page_no};
                Page *left_page = buffer_pool_manager_->FetchPage(left_pid);
                current_page->WUnlatch();
                relatch_pair(left_pid, left_page, current_pid, current_page);
                BTreeNodePage left_node(left_page, key_type_, max_varchar_len_);
                BTreeNodePage child_node2(current_page, key_type_, max_varchar_len_);
                std::vector<InternalEntry> merged = left_node.ReadInternalEntries();
                merged.push_back(InternalEntry{parent_entries[static_cast<std::size_t>(child_pos - 1)].key,
                                               child_node2.GetLeftmostChild()});
                std::vector<InternalEntry> child_entries = child_node2.ReadInternalEntries();
                merged.insert(merged.end(), child_entries.begin(), child_entries.end());
                if (!CanFitInternalEntries(left_node.GetLeftmostChild(), merged)) {
                    left_page->WUnlatch();
                    buffer_pool_manager_->UnpinPage(left_pid, false);
                    ReleaseWritePath(buffer_pool_manager_, path);
                    ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, false);
                    ReleaseWriteFrame(buffer_pool_manager_, parent_frame, false);
                    throw std::runtime_error("Merged internal node does not fit in one page");
                }
                std::vector<char> child_high_key2;
                bool child_has_high_key2 = child_node2.HasHighKey();
                if (child_has_high_key2) child_high_key2 = child_node2.GetHighKey();
                rewrite_internal(left_pid, left_page, left_node.GetLeftmostChild(), merged, child_node2.GetRightLink(),
                                 child_has_high_key2 ? &child_high_key2 : nullptr, LogRecordType::BTREE_MERGE);
                retire_internal_page(current_pid, current_page, left_pid.page_no, child_node2.GetRightLink(),
                                     child_has_high_key2 ? &child_high_key2 : nullptr);
                parent_entries.erase(parent_entries.begin() + (child_pos - 1));
                retired_page_id = current_pid;
                fixed = true;
                left_page->WUnlatch();
                buffer_pool_manager_->UnpinPage(left_pid, true);
            }

            if (!fixed && right_sibling_page_no != INVALID_PAGE_NO) {
                PageId right_pid{index_relation_id_, right_sibling_page_no};
                Page *right_page = buffer_pool_manager_->FetchPage(right_pid);
                right_page->WLatch();
                BTreeNodePage right_node(right_page, key_type_, max_varchar_len_);
                BTreeNodePage child_node2(current_page, key_type_, max_varchar_len_);
                std::vector<InternalEntry> merged = child_node2.ReadInternalEntries();
                merged.push_back(InternalEntry{parent_entries[static_cast<std::size_t>(child_pos)].key,
                                               right_node.GetLeftmostChild()});
                std::vector<InternalEntry> right_entries = right_node.ReadInternalEntries();
                merged.insert(merged.end(), right_entries.begin(), right_entries.end());
                if (!CanFitInternalEntries(child_node2.GetLeftmostChild(), merged)) {
                    right_page->WUnlatch();
                    buffer_pool_manager_->UnpinPage(right_pid, false);
                    ReleaseWritePath(buffer_pool_manager_, path);
                    ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, false);
                    ReleaseWriteFrame(buffer_pool_manager_, parent_frame, false);
                    throw std::runtime_error("Merged internal node does not fit in one page");
                }
                std::vector<char> right_high_key;
                bool right_has_high_key = right_node.HasHighKey();
                if (right_has_high_key) right_high_key = right_node.GetHighKey();
                rewrite_internal(current_pid, current_page, child_node2.GetLeftmostChild(), merged, right_node.GetRightLink(),
                                 right_has_high_key ? &right_high_key : nullptr, LogRecordType::BTREE_MERGE);
                retire_internal_page(right_pid, right_page, current_pid.page_no, right_node.GetRightLink(),
                                     right_has_high_key ? &right_high_key : nullptr);
                parent_entries.erase(parent_entries.begin() + child_pos);
                retired_page_id = right_pid;
                fixed = true;
                right_page->WUnlatch();
                buffer_pool_manager_->UnpinPage(right_pid, true);
            }
        }

        if (!fixed) {
            ReleaseWritePath(buffer_pool_manager_, path);
            ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, false);
            ReleaseWriteFrame(buffer_pool_manager_, parent_frame, false);
            throw std::runtime_error("Failed to rebalance underflowed B+ tree node");
        }

        bool parent_is_root = parent_pid.page_no == root_pid.page_no;
        if (parent_is_root && parent_entries.empty()) {
            retire_internal_page(parent_pid, parent_page, parent_leftmost, INVALID_PAGE_NO, nullptr);
            BTreeMetaPageData meta2 = ReadMetaPage();
            if (meta2.tree_height > 1) {
                meta2.root_page_no = parent_leftmost;
                meta2.tree_height--;
                WriteMetaPage(meta2, txn);
            }
            ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, child_dirty);
            ReleaseWritePath(buffer_pool_manager_, path);
            ReleaseWriteFrame(buffer_pool_manager_, parent_frame, true);
            if (retired_page_id.has_value()) schedule_retire(*retired_page_id);
            schedule_retire(parent_pid);
            return true;
        }

        rewrite_internal(parent_pid, parent_page, parent_leftmost, parent_entries, parent_right,
                         parent_has_high_key ? &parent_high_key : nullptr, LogRecordType::BTREE_REBALANCE);
        ReleaseWriteFrame(buffer_pool_manager_, {current_pid, current_page}, child_dirty);
        if (retired_page_id.has_value()) schedule_retire(*retired_page_id);

        if (parent_is_root || !IsInternalUnderfull(parent_entries)) {
            ReleaseWritePath(buffer_pool_manager_, path);
            ReleaseWriteFrame(buffer_pool_manager_, parent_frame, true);
            return true;
        }

        current_pid = parent_pid;
        current_page = parent_page;
    }
}


}  // namespace simpledb
