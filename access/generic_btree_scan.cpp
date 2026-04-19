#include "generic_btree_helpers.h"

#include <optional>
#include <stdexcept>

#include "../execution/expressions.h"

namespace simpledb {
using namespace generic_btree_helpers;

// Read-only search/scan paths are separated from the update algorithms so the
// lookup story stays easy to follow: normalize cursor, descend, walk leaves,
// decode entries, return RIDs.

std::vector<GenericBTreeIndex::KeyRidEntry> GenericBTreeIndex::FullScanEntries() const {
    auto op = buffer_pool_manager_->GetPageRetireManager()->Guard();
    BTreeMetaPageData meta = ReadMetaPage();
    PageNo current = meta.root_page_no;

    while (true) {
        PageId pid{definition_.index_relation_id, current};
        Page *page = buffer_pool_manager_->FetchPage(pid);
        if (page == nullptr) throw std::runtime_error("Failed to fetch generic B+ tree page during full scan");
        page->RLatch();
        GenericBTreeNodePage node(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
        if (node.IsLeaf()) {
            page->RUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            break;
        }
        current = node.GetLeftmostChild();
        page->RUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
    }

    std::vector<KeyRidEntry> result;
    while (current != INVALID_PAGE_NO) {
        PageId pid{definition_.index_relation_id, current};
        Page *page = buffer_pool_manager_->FetchPage(pid);
        if (page == nullptr) throw std::runtime_error("Failed to fetch generic B+ tree leaf during full scan");
        page->RLatch();
        GenericBTreeNodePage node(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
        std::vector<GenericLeafEntry> entries = node.ReadLeafEntries();
        for (const auto &entry : entries) {
            result.push_back(KeyRidEntry{CompositeKeyCodec::DecodeKey(entry.key, definition_.key_columns), entry.rid});
        }
        PageNo next = node.GetRightLink();
        page->RUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        current = next;
    }
    return result;
}

std::vector<RID> GenericBTreeIndex::FullScanRids() const {
    auto op = buffer_pool_manager_->GetPageRetireManager()->Guard();
    BTreeMetaPageData meta = ReadMetaPage();
    PageNo current = meta.root_page_no;

    while (true) {
        PageId pid{definition_.index_relation_id, current};
        Page *page = buffer_pool_manager_->FetchPage(pid);
        if (page == nullptr) throw std::runtime_error("Failed to fetch generic B+ tree page during full scan");
        page->RLatch();
        GenericBTreeNodePage node(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
        if (node.IsLeaf()) {
            page->RUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
            break;
        }
        current = node.GetLeftmostChild();
        page->RUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
    }

    std::vector<RID> result;
    while (current != INVALID_PAGE_NO) {
        PageId pid{definition_.index_relation_id, current};
        Page *page = buffer_pool_manager_->FetchPage(pid);
        if (page == nullptr) throw std::runtime_error("Failed to fetch generic B+ tree leaf during full scan");
        page->RLatch();
        GenericBTreeNodePage node(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
        std::vector<GenericLeafEntry> entries = node.ReadLeafEntries();
        for (const auto &entry : entries) result.push_back(entry.rid);
        PageNo next = node.GetRightLink();
        page->RUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        current = next;
    }
    return result;
}


std::vector<GenericBTreeIndex::KeyRidEntry> GenericBTreeIndex::ScanPrefixRangeEntries(const PrefixScanSpec &spec) const {
    auto op = buffer_pool_manager_->GetPageRetireManager()->Guard();
    const std::size_t key_cols = definition_.key_columns.size();
    if (spec.equality_prefix.size() > key_cols) {
        throw std::runtime_error("Equality prefix is longer than index definition");
    }
    if ((spec.lower_bound.has_value() || spec.upper_bound.has_value()) && spec.equality_prefix.size() >= key_cols) {
        throw std::runtime_error("Range bound requires one more index key column after equality prefix");
    }

    std::vector<KeyRidEntry> result;
    PageNo current = INVALID_PAGE_NO;

    if (spec.equality_prefix.empty() && !spec.lower_bound.has_value()) {
        BTreeMetaPageData meta = ReadMetaPage();
        current = meta.root_page_no;
        while (true) {
            PageId pid{definition_.index_relation_id, current};
            Page *page = buffer_pool_manager_->FetchPage(pid);
            if (page == nullptr) throw std::runtime_error("Failed to fetch generic B+ tree page during prefix scan");
            page->RLatch();
            NormalizeReadCursor(pid, page);
            GenericBTreeNodePage node(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
            if (node.IsLeaf()) {
                page->RUnlatch();
                buffer_pool_manager_->UnpinPage(pid, false);
                break;
            }
            current = node.GetLeftmostChild();
            page->RUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
        }
    } else {
        std::vector<Value> start_key_values;
        start_key_values.reserve(key_cols);
        for (const Value &v : spec.equality_prefix) start_key_values.push_back(v);
        if (start_key_values.size() < key_cols) {
            const auto &range_col = definition_.key_columns[start_key_values.size()];
            if (spec.lower_bound.has_value()) start_key_values.push_back(*spec.lower_bound);
            else {
                switch (range_col.type) {
                    case TypeId::BOOLEAN: start_key_values.push_back(Value(false)); break;
                    case TypeId::INT32: start_key_values.push_back(Value(std::numeric_limits<int32_t>::min())); break;
                    case TypeId::INT64: start_key_values.push_back(Value(std::numeric_limits<int64_t>::min())); break;
                    case TypeId::DOUBLE: start_key_values.push_back(Value(-std::numeric_limits<double>::infinity())); break;
                    case TypeId::VARCHAR: start_key_values.push_back(Value(std::string())); break;
                    default: throw std::runtime_error("Unsupported type while building generic scan lower bound");
                }
            }
        }
        while (start_key_values.size() < key_cols) {
            const auto &col = definition_.key_columns[start_key_values.size()];
            switch (col.type) {
                case TypeId::BOOLEAN: start_key_values.push_back(Value(false)); break;
                case TypeId::INT32: start_key_values.push_back(Value(std::numeric_limits<int32_t>::min())); break;
                case TypeId::INT64: start_key_values.push_back(Value(std::numeric_limits<int64_t>::min())); break;
                case TypeId::DOUBLE: start_key_values.push_back(Value(-std::numeric_limits<double>::infinity())); break;
                case TypeId::VARCHAR: start_key_values.push_back(Value(std::string())); break;
                default: throw std::runtime_error("Unsupported type while padding generic scan lower bound");
            }
        }
        current = FindLeafPageNoEncoded(EncodeKey(start_key_values));
    }

    const std::size_t range_col_idx = spec.equality_prefix.size();
    while (current != INVALID_PAGE_NO) {
        PageId pid{definition_.index_relation_id, current};
        Page *page = buffer_pool_manager_->FetchPage(pid);
        if (page == nullptr) throw std::runtime_error("Failed to fetch generic B+ tree leaf during prefix scan");
        page->RLatch();
        NormalizeReadCursor(pid, page);
        GenericBTreeNodePage node(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
        std::vector<GenericLeafEntry> entries = node.ReadLeafEntries();

        bool should_stop = false;
        for (const auto &entry : entries) {
            std::vector<Value> key_values = CompositeKeyCodec::DecodeKey(entry.key, definition_.key_columns);

            bool prefix_less = false;
            bool prefix_greater = false;
            bool prefix_match = true;
            for (std::size_t i = 0; i < spec.equality_prefix.size(); ++i) {
                int cmp = CompareValues(key_values[i], spec.equality_prefix[i]);
                if (cmp < 0) { prefix_less = true; prefix_match = false; break; }
                if (cmp > 0) { prefix_greater = true; prefix_match = false; break; }
            }
            if (prefix_less) continue;
            if (prefix_greater) { should_stop = true; break; }
            if (!prefix_match) continue;

            if (range_col_idx < key_values.size() && (spec.lower_bound.has_value() || spec.upper_bound.has_value())) {
                const Value &range_value = key_values[range_col_idx];
                if (spec.lower_bound.has_value()) {
                    int cmp = CompareValues(range_value, *spec.lower_bound);
                    if (cmp < 0 || (cmp == 0 && !spec.lower_inclusive)) continue;
                }
                if (spec.upper_bound.has_value()) {
                    int cmp = CompareValues(range_value, *spec.upper_bound);
                    if (cmp > 0 || (cmp == 0 && !spec.upper_inclusive)) {
                        should_stop = true;
                        break;
                    }
                }
            }

            result.push_back(KeyRidEntry{std::move(key_values), entry.rid});
        }

        PageNo next = node.GetRightLink();
        page->RUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        if (should_stop) break;
        current = next;
    }

    return result;
}

std::vector<RID> GenericBTreeIndex::ScanPrefixRange(const PrefixScanSpec &spec) const {
    auto op = buffer_pool_manager_->GetPageRetireManager()->Guard();
    const std::size_t key_cols = definition_.key_columns.size();
    if (spec.equality_prefix.size() > key_cols) {
        throw std::runtime_error("Equality prefix is longer than index definition");
    }
    if ((spec.lower_bound.has_value() || spec.upper_bound.has_value()) && spec.equality_prefix.size() >= key_cols) {
        throw std::runtime_error("Range bound requires one more index key column after equality prefix");
    }

    std::vector<RID> result;
    PageNo current = INVALID_PAGE_NO;

    if (spec.equality_prefix.empty() && !spec.lower_bound.has_value()) {
        BTreeMetaPageData meta = ReadMetaPage();
        current = meta.root_page_no;
        while (true) {
            PageId pid{definition_.index_relation_id, current};
            Page *page = buffer_pool_manager_->FetchPage(pid);
            if (page == nullptr) throw std::runtime_error("Failed to fetch generic B+ tree page during prefix scan");
            page->RLatch();
            NormalizeReadCursor(pid, page);
            GenericBTreeNodePage node(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
            if (node.IsLeaf()) {
                page->RUnlatch();
                buffer_pool_manager_->UnpinPage(pid, false);
                break;
            }
            current = node.GetLeftmostChild();
            page->RUnlatch();
            buffer_pool_manager_->UnpinPage(pid, false);
        }
    } else {
        std::vector<Value> start_key_values;
        start_key_values.reserve(key_cols);
        for (const Value &v : spec.equality_prefix) start_key_values.push_back(v);
        if (start_key_values.size() < key_cols) {
            const auto &range_col = definition_.key_columns[start_key_values.size()];
            if (spec.lower_bound.has_value()) start_key_values.push_back(*spec.lower_bound);
            else {
                switch (range_col.type) {
                    case TypeId::BOOLEAN: start_key_values.push_back(Value(false)); break;
                    case TypeId::INT32: start_key_values.push_back(Value(std::numeric_limits<int32_t>::min())); break;
                    case TypeId::INT64: start_key_values.push_back(Value(std::numeric_limits<int64_t>::min())); break;
                    case TypeId::DOUBLE: start_key_values.push_back(Value(-std::numeric_limits<double>::infinity())); break;
                    case TypeId::VARCHAR: start_key_values.push_back(Value(std::string())); break;
                    default: throw std::runtime_error("Unsupported type while building generic scan lower bound");
                }
            }
        }
        while (start_key_values.size() < key_cols) {
            const auto &col = definition_.key_columns[start_key_values.size()];
            switch (col.type) {
                case TypeId::BOOLEAN: start_key_values.push_back(Value(false)); break;
                case TypeId::INT32: start_key_values.push_back(Value(std::numeric_limits<int32_t>::min())); break;
                case TypeId::INT64: start_key_values.push_back(Value(std::numeric_limits<int64_t>::min())); break;
                case TypeId::DOUBLE: start_key_values.push_back(Value(-std::numeric_limits<double>::infinity())); break;
                case TypeId::VARCHAR: start_key_values.push_back(Value(std::string())); break;
                default: throw std::runtime_error("Unsupported type while padding generic scan lower bound");
            }
        }
        current = FindLeafPageNoEncoded(EncodeKey(start_key_values));
    }

    const std::size_t range_col_idx = spec.equality_prefix.size();
    while (current != INVALID_PAGE_NO) {
        PageId pid{definition_.index_relation_id, current};
        Page *page = buffer_pool_manager_->FetchPage(pid);
        if (page == nullptr) throw std::runtime_error("Failed to fetch generic B+ tree leaf during prefix scan");
        page->RLatch();
        NormalizeReadCursor(pid, page);
        GenericBTreeNodePage node(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
        std::vector<GenericLeafEntry> entries = node.ReadLeafEntries();

        bool should_stop = false;
        for (const auto &entry : entries) {
            std::vector<Value> key_values = CompositeKeyCodec::DecodeKey(entry.key, definition_.key_columns);

            bool prefix_less = false;
            bool prefix_greater = false;
            bool prefix_match = true;
            for (std::size_t i = 0; i < spec.equality_prefix.size(); ++i) {
                int cmp = CompareValues(key_values[i], spec.equality_prefix[i]);
                if (cmp < 0) { prefix_less = true; prefix_match = false; break; }
                if (cmp > 0) { prefix_greater = true; prefix_match = false; break; }
            }
            if (prefix_less) continue;
            if (prefix_greater) { should_stop = true; break; }
            if (!prefix_match) continue;

            if (range_col_idx < key_values.size() && (spec.lower_bound.has_value() || spec.upper_bound.has_value())) {
                const Value &range_value = key_values[range_col_idx];
                if (spec.lower_bound.has_value()) {
                    int cmp = CompareValues(range_value, *spec.lower_bound);
                    if (cmp < 0 || (cmp == 0 && !spec.lower_inclusive)) continue;
                }
                if (spec.upper_bound.has_value()) {
                    int cmp = CompareValues(range_value, *spec.upper_bound);
                    if (cmp > 0 || (cmp == 0 && !spec.upper_inclusive)) {
                        should_stop = true;
                        break;
                    }
                }
            }

            result.push_back(entry.rid);
        }

        PageNo next = node.GetRightLink();
        page->RUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        if (should_stop) break;
        current = next;
    }

    return result;
}


std::vector<GenericBTreeIndex::KeyRidEntry> GenericBTreeIndex::SearchExactEntries(const std::vector<Value> &key_values) const {
    auto op = buffer_pool_manager_->GetPageRetireManager()->Guard();
    std::vector<char> encoded = EncodeKey(key_values);
    PageNo leaf_page_no = FindLeafPageNoEncoded(encoded);
    std::vector<KeyRidEntry> result;
    PageNo current = leaf_page_no;

    while (current != INVALID_PAGE_NO) {
        PageId pid{definition_.index_relation_id, current};
        Page *page = buffer_pool_manager_->FetchPage(pid);
        if (page == nullptr) throw std::runtime_error("Failed to fetch leaf page during search");
        page->RLatch();
        NormalizeReadCursor(pid, page);
        GenericBTreeNodePage node(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
        std::vector<GenericLeafEntry> entries = node.ReadLeafEntries();

        bool continue_right = false;
        bool stop = false;
        for (const auto &entry : entries) {
            int cmp = CompareKeys(entry.key, encoded);
            if (cmp < 0) continue;
            if (cmp == 0) {
                result.push_back(KeyRidEntry{CompositeKeyCodec::DecodeKey(entry.key, definition_.key_columns), entry.rid});
                continue_right = (node.GetRightLink() != INVALID_PAGE_NO);
                continue;
            }
            stop = true;
            break;
        }

        PageNo next = INVALID_PAGE_NO;
        if (!stop && continue_right && !entries.empty() && CompareKeys(entries.back().key, encoded) == 0) {
            next = node.GetRightLink();
        }

        page->RUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        current = next;
    }

    return result;
}

std::vector<RID> GenericBTreeIndex::SearchExact(const std::vector<Value> &key_values) const {
    auto op = buffer_pool_manager_->GetPageRetireManager()->Guard();
    std::vector<char> encoded = EncodeKey(key_values);
    PageNo leaf_page_no = FindLeafPageNoEncoded(encoded);
    std::vector<RID> result;
    PageNo current = leaf_page_no;

    while (current != INVALID_PAGE_NO) {
        PageId pid{definition_.index_relation_id, current};
        Page *page = buffer_pool_manager_->FetchPage(pid);
        if (page == nullptr) throw std::runtime_error("Failed to fetch leaf page during search");
        page->RLatch();
        NormalizeReadCursor(pid, page);
        GenericBTreeNodePage node(page, max_key_bytes_, [this](const std::vector<char> &a, const std::vector<char> &b){ return CompareKeys(a, b); });
        std::vector<GenericLeafEntry> entries = node.ReadLeafEntries();

        bool continue_right = false;
        bool stop = false;
        for (const auto &entry : entries) {
            int cmp = CompareKeys(entry.key, encoded);
            if (cmp < 0) continue;
            if (cmp == 0) {
                result.push_back(entry.rid);
                continue_right = (node.GetRightLink() != INVALID_PAGE_NO);
                continue;
            }
            stop = true;
            break;
        }

        PageNo next = INVALID_PAGE_NO;
        if (!stop && continue_right && !entries.empty() && CompareKeys(entries.back().key, encoded) == 0) {
            next = node.GetRightLink();
        }

        page->RUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);
        current = next;
    }

    return result;
}
}  // namespace simpledb
