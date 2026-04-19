#include "btree_iterator.h"

#include <stdexcept>

namespace simpledb {

BTreeIndexIterator::BTreeIndexIterator(const BTreeIndex *tree)
    : tree_(tree), current_leaf_page_no_(tree->GetLeftmostLeafPageNo()), current_entry_index_(0),
      at_end_(false), has_lower_bound_(false) {
    AdvanceToNextValid();
}

BTreeIndexIterator::BTreeIndexIterator(const BTreeIndex *tree, const Value &lower_bound)
    : tree_(tree), current_leaf_page_no_(tree->FindLeafPageNo(lower_bound)), current_entry_index_(0),
      at_end_(false), has_lower_bound_(true),
      lower_bound_key_(IndexKeyUtil::EncodeValue(lower_bound, tree->GetKeyType(), tree->GetMaxVarcharLength())) {
    AdvanceToNextValid();
}

bool BTreeIndexIterator::HasNext() const { return !at_end_; }

std::pair<Value, RID> BTreeIndexIterator::Next() {
    auto op = tree_->GetBufferPoolManager()->GetPageRetireManager()->Guard();
    if (at_end_) throw std::runtime_error("BTreeIndexIterator::Next called at end");

    while (true) {
        PageId pid{tree_->GetIndexRelationId(), current_leaf_page_no_};
        Page *page = tree_->GetBufferPoolManager()->FetchPage(pid);
        if (page == nullptr) throw std::runtime_error("Failed to fetch leaf page in iterator");
        page->RLatch();
        BTreeNodePage node(page, tree_->GetKeyType(), tree_->GetMaxVarcharLength());

        if (node.IsRetired() && node.GetRightLink() != INVALID_PAGE_NO) {
            PageNo next_page_no = node.GetRightLink();
            page->RUnlatch();
            tree_->GetBufferPoolManager()->UnpinPage(pid, false);
            current_leaf_page_no_ = next_page_no;
            current_entry_index_ = 0;
            AdvanceToNextValid();
            if (at_end_) throw std::runtime_error("BTreeIndexIterator::Next called at end");
            continue;
        }

        if (!node.IsLeaf()) {
            page->RUnlatch();
            tree_->GetBufferPoolManager()->UnpinPage(pid, false);
            current_entry_index_ = 0;
            AdvanceToNextValid();
            if (at_end_) throw std::runtime_error("BTreeIndexIterator::Next called at end");
            continue;
        }

        std::vector<LeafEntry> entries = node.ReadLeafEntries();
        if (current_entry_index_ >= entries.size()) {
            page->RUnlatch();
            tree_->GetBufferPoolManager()->UnpinPage(pid, false);
            AdvanceToNextValid();
            if (at_end_) throw std::runtime_error("BTreeIndexIterator::Next called at end");
            continue;
        }

        const LeafEntry &entry = entries[current_entry_index_];
        Value key = IndexKeyUtil::DecodeValue(tree_->GetKeyType(), entry.key.data(), static_cast<uint32_t>(entry.key.size()));
        RID rid = entry.rid;
        page->RUnlatch();
        tree_->GetBufferPoolManager()->UnpinPage(pid, false);
        current_entry_index_++;
        AdvanceToNextValid();
        return {key, rid};
    }
}

void BTreeIndexIterator::AdvanceToNextValid() {
    auto op = tree_->GetBufferPoolManager()->GetPageRetireManager()->Guard();
    while (current_leaf_page_no_ != INVALID_PAGE_NO) {
        PageId pid{tree_->GetIndexRelationId(), current_leaf_page_no_};
        Page *page = tree_->GetBufferPoolManager()->FetchPage(pid);
        if (page == nullptr) throw std::runtime_error("Failed to fetch leaf page in iterator advance");
        page->RLatch();

        while (true) {
            BTreeNodePage node(page, tree_->GetKeyType(), tree_->GetMaxVarcharLength());
            if (node.IsRetired() && node.GetRightLink() != INVALID_PAGE_NO) {
            PageNo next_page_no = node.GetRightLink();
            page->RUnlatch();
            tree_->GetBufferPoolManager()->UnpinPage(pid, false);
            current_leaf_page_no_ = next_page_no;
            current_entry_index_ = 0;
            AdvanceToNextValid();
            if (at_end_) throw std::runtime_error("BTreeIndexIterator::Next called at end");
            continue;
        }

        if (!node.IsLeaf()) {
                PageNo child = node.GetLeftmostChild();
                page->RUnlatch();
                tree_->GetBufferPoolManager()->UnpinPage(pid, false);
                if (child == INVALID_PAGE_NO) {
                    current_leaf_page_no_ = INVALID_PAGE_NO;
                    at_end_ = true;
                    return;
                }
                current_leaf_page_no_ = child;
                current_entry_index_ = 0;
                break;
            }
            std::vector<LeafEntry> entries = node.ReadLeafEntries();

            while (current_entry_index_ < entries.size()) {
                if (has_lower_bound_) {
                    int cmp = IndexKeyUtil::CompareEncoded(tree_->GetKeyType(), entries[current_entry_index_].key, lower_bound_key_);
                    if (cmp < 0) {
                        current_entry_index_++;
                        continue;
                    }
                    has_lower_bound_ = false;
                }
                page->RUnlatch();
                tree_->GetBufferPoolManager()->UnpinPage(pid, false);
                return;
            }

            PageNo right_link = node.GetRightLink();
            if (right_link == INVALID_PAGE_NO) {
                page->RUnlatch();
                tree_->GetBufferPoolManager()->UnpinPage(pid, false);
                current_leaf_page_no_ = INVALID_PAGE_NO;
                at_end_ = true;
                return;
            }

            PageId right_pid{tree_->GetIndexRelationId(), right_link};
            Page *right_page = tree_->GetBufferPoolManager()->FetchPage(right_pid);
            if (right_page == nullptr) {
                page->RUnlatch();
                tree_->GetBufferPoolManager()->UnpinPage(pid, false);
                throw std::runtime_error("Failed to fetch right sibling in iterator advance");
            }
            right_page->RLatch();
            page->RUnlatch();
            tree_->GetBufferPoolManager()->UnpinPage(pid, false);
            pid = right_pid;
            page = right_page;
            current_leaf_page_no_ = right_link;
            current_entry_index_ = 0;
        }
    }
    at_end_ = true;
}

}  // namespace simpledb
