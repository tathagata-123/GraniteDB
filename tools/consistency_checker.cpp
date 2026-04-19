#include "consistency_checker.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "../access/btree_iterator.h"
#include "../access/btree_page.h"
#include "../access/composite_key_codec.h"
#include "../access/generic_btree_page.h"
#include "../access/heap_file_iterator.h"
#include "../execution/expressions.h"
#include "../storage/slotted_page.h"

namespace simpledb {

namespace {

std::string RidToString(const RID &rid) {
    std::ostringstream out;
    out << "(" << rid.page_no << ", " << rid.slot_no << ")";
    return out.str();
}

std::string BytesToHex(const std::vector<char> &bytes) {
    static const char *kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char ch : bytes) {
        out.push_back(kHex[(ch >> 4) & 0xF]);
        out.push_back(kHex[ch & 0xF]);
    }
    return out;
}

std::string EntryToken(const std::vector<Value> &key_values, const RID &rid) {
    std::ostringstream out;
    for (const Value &value : key_values) {
        out << static_cast<int>(value.GetTypeId()) << "#" << value.ToString() << "|";
    }
    out << rid.page_no << ":" << rid.slot_no;
    return out.str();
}

std::string EncodedEntryToken(const std::vector<char> &encoded_key, const RID &rid) {
    std::ostringstream out;
    out << BytesToHex(encoded_key) << "|" << rid.page_no << ":" << rid.slot_no;
    return out.str();
}

std::vector<Value> ExtractKeyValues(const Tuple &tuple,
                                    const std::vector<std::size_t> &key_column_indexes) {
    std::vector<Value> out;
    out.reserve(key_column_indexes.size());
    for (std::size_t column_idx : key_column_indexes) {
        out.push_back(tuple.GetValue(column_idx));
    }
    return out;
}

bool ContainsRID(const std::vector<RID> &rids, const RID &target) {
    for (const auto &rid : rids) {
        if (rid == target) {
            return true;
        }
    }
    return false;
}

template <typename CompareFn>
bool CheckStrictlySortedEncoded(const std::vector<std::vector<char>> &keys,
                                CompareFn compare_fn,
                                std::string *error,
                                const std::string &what,
                                PageNo page_no) {
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (compare_fn(keys[i - 1], keys[i]) >= 0) {
            if (error != nullptr) {
                *error = what + " keys are not strictly increasing on page " + std::to_string(page_no);
            }
            return false;
        }
    }
    return true;
}

template <typename CompareFn>
bool CheckNonDecreasingEncoded(const std::vector<std::vector<char>> &keys,
                               CompareFn compare_fn,
                               std::string *error,
                               const std::string &what,
                               PageNo page_no) {
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (compare_fn(keys[i - 1], keys[i]) > 0) {
            if (error != nullptr) {
                *error = what + " keys are not nondecreasing on page " + std::to_string(page_no);
            }
            return false;
        }
    }
    return true;
}

bool VerifyScalarTreeShape(const BTreeIndex &index, std::string *error) {
    BufferPoolManager *bpm = index.GetBufferPoolManager();
    RelationId rel = index.GetIndexRelationId();
    const PageNo root_page_no = index.GetRootPageNo();
    const uint32_t num_pages = bpm->GetDiskManager()->GetNumPages(rel);
    const auto compare = [&](const std::vector<char> &lhs, const std::vector<char> &rhs) {
        return IndexKeyUtil::CompareEncoded(index.GetKeyType(), lhs, rhs);
    };

    std::set<PageNo> live_pages;
    std::vector<PageNo> dfs_leaf_order;
    std::vector<std::string> dfs_tokens;

    std::function<bool(PageNo, const std::optional<std::vector<char>> &, const std::optional<std::vector<char>> &)> walk;
    walk = [&](PageNo page_no,
               const std::optional<std::vector<char>> &low_key,
               const std::optional<std::vector<char>> &high_key) -> bool {
        if (page_no == INVALID_PAGE_NO) {
            if (error != nullptr) *error = "Encountered invalid child pointer during live-tree traversal";
            return false;
        }
        if (!live_pages.insert(page_no).second) {
            if (error != nullptr) *error = "Live tree traversal revisited page " + std::to_string(page_no);
            return false;
        }
        PageId pid{rel, page_no};
        Page *page = bpm->FetchPage(pid);
        if (page == nullptr) {
            if (error != nullptr) *error = "Failed to fetch live B+ tree page " + std::to_string(page_no);
            return false;
        }
        page->RLatch();
        BTreeNodePage node(page, index.GetKeyType(), index.GetMaxVarcharLength());
        if (node.IsRetired()) {
            page->RUnlatch();
            bpm->UnpinPage(pid, false);
            if (error != nullptr) *error = "Retired page " + std::to_string(page_no) + " is reachable from the live root";
            return false;
        }

        if (node.IsLeaf()) {
            std::vector<LeafEntry> entries = node.ReadLeafEntries();
            std::vector<std::vector<char>> keys;
            keys.reserve(entries.size());
            for (const auto &entry : entries) keys.push_back(entry.key);
            if (!CheckNonDecreasingEncoded(keys, compare, error, "Leaf", page_no)) {
                page->RUnlatch();
                bpm->UnpinPage(pid, false);
                return false;
            }
            for (const auto &entry : entries) {
                if (low_key.has_value() && compare(entry.key, *low_key) < 0) {
                    page->RUnlatch();
                    bpm->UnpinPage(pid, false);
                    if (error != nullptr) *error = "Leaf key fell below its lower fence on page " + std::to_string(page_no);
                    return false;
                }
                if (high_key.has_value() && compare(entry.key, *high_key) >= 0) {
                    page->RUnlatch();
                    bpm->UnpinPage(pid, false);
                    if (error != nullptr) *error = "Leaf key crossed its upper fence on page " + std::to_string(page_no);
                    return false;
                }
                dfs_tokens.push_back(EncodedEntryToken(entry.key, entry.rid));
            }
            dfs_leaf_order.push_back(page_no);
            page->RUnlatch();
            bpm->UnpinPage(pid, false);
            return true;
        }

        std::vector<InternalEntry> entries = node.ReadInternalEntries();
        std::vector<std::vector<char>> keys;
        keys.reserve(entries.size());
        for (const auto &entry : entries) keys.push_back(entry.key);
        if (!CheckStrictlySortedEncoded(keys, compare, error, "Internal", page_no)) {
            page->RUnlatch();
            bpm->UnpinPage(pid, false);
            return false;
        }

        std::vector<PageNo> children;
        children.reserve(entries.size() + 1);
        children.push_back(node.GetLeftmostChild());
        for (const auto &entry : entries) children.push_back(entry.child);
        page->RUnlatch();
        bpm->UnpinPage(pid, false);

        for (std::size_t i = 0; i < children.size(); ++i) {
            std::optional<std::vector<char>> child_low = (i == 0 ? low_key : std::optional<std::vector<char>>(entries[i - 1].key));
            std::optional<std::vector<char>> child_high = (i == entries.size() ? high_key : std::optional<std::vector<char>>(entries[i].key));
            if (!walk(children[i], child_low, child_high)) return false;
        }
        return true;
    };

    if (!walk(root_page_no, std::nullopt, std::nullopt)) {
        return false;
    }

    std::vector<PageNo> chain_live_leafs;
    PageNo current = index.GetLeftmostLeafPageNo();
    std::size_t hop_budget = static_cast<std::size_t>(num_pages) + 4;
    while (current != INVALID_PAGE_NO && hop_budget-- > 0) {
        PageId pid{rel, current};
        Page *page = bpm->FetchPage(pid);
        if (page == nullptr) {
            if (error != nullptr) *error = "Failed to fetch leaf-chain page " + std::to_string(current);
            return false;
        }
        page->RLatch();
        BTreeNodePage node(page, index.GetKeyType(), index.GetMaxVarcharLength());
        if (!node.IsLeaf()) {
            page->RUnlatch();
            bpm->UnpinPage(pid, false);
            if (error != nullptr) *error = "Leaf chain reached non-leaf page " + std::to_string(current);
            return false;
        }
        PageNo next = node.GetRightLink();
        if (!node.IsRetired()) chain_live_leafs.push_back(current);
        page->RUnlatch();
        bpm->UnpinPage(pid, false);
        current = next;
    }
    if (current != INVALID_PAGE_NO) {
        if (error != nullptr) *error = "Leaf chain did not terminate within the page budget";
        return false;
    }
    if (dfs_leaf_order != chain_live_leafs) {
        if (error != nullptr) *error = "Leaf chain order does not match DFS leaf order";
        return false;
    }

    std::vector<std::string> iter_tokens;
    BTreeIndexIterator it(&index);
    while (it.HasNext()) {
        auto [key, rid] = it.Next();
        std::vector<char> encoded = IndexKeyUtil::EncodeValue(key, index.GetKeyType(), index.GetMaxVarcharLength());
        iter_tokens.push_back(EncodedEntryToken(encoded, rid));
    }
    auto sorted_dfs = dfs_tokens;
    auto sorted_iter = iter_tokens;
    std::sort(sorted_dfs.begin(), sorted_dfs.end());
    std::sort(sorted_iter.begin(), sorted_iter.end());
    if (sorted_dfs != sorted_iter) {
        if (error != nullptr) *error = "Iterator-visible entries do not match the live leaf contents";
        return false;
    }
    if (std::unordered_set<std::string>(sorted_dfs.begin(), sorted_dfs.end()).size() != sorted_dfs.size()) {
        if (error != nullptr) *error = "Duplicate exact (key,rid) entries found in scalar B+ tree";
        return false;
    }

    for (PageNo page_no = 1; page_no < num_pages; ++page_no) {
        PageId pid{rel, page_no};
        Page *page = bpm->FetchPage(pid);
        if (page == nullptr) continue;
        page->RLatch();
        SlottedPage sp(page);
        if (sp.IsInitialized() && (sp.GetPageType() == PageType::BTREE_LEAF || sp.GetPageType() == PageType::BTREE_INTERNAL)) {
            BTreeNodePage node(page, index.GetKeyType(), index.GetMaxVarcharLength());
            if (page_no == root_page_no && node.IsRetired()) {
                page->RUnlatch();
                bpm->UnpinPage(pid, false);
                if (error != nullptr) *error = "Meta root points to a retired page";
                return false;
            }
            if (node.IsRetired() && live_pages.count(page_no) != 0) {
                page->RUnlatch();
                bpm->UnpinPage(pid, false);
                if (error != nullptr) *error = "Retired page appears in the live-page set";
                return false;
            }
        }
        page->RUnlatch();
        bpm->UnpinPage(pid, false);
    }

    return true;
}

bool VerifyGenericTreeShape(const GenericBTreeIndex &index, std::string *error) {
    BufferPoolManager *bpm = index.GetBufferPoolManager();
    RelationId rel = index.GetIndexRelationId();
    const PageNo root_page_no = index.GetRootPageNo();
    const uint32_t num_pages = bpm->GetDiskManager()->GetNumPages(rel);
    const auto &definition = index.GetDefinition();
    const auto compare = [&](const std::vector<char> &lhs, const std::vector<char> &rhs) {
        return CompositeKeyCodec::CompareEncoded(lhs, rhs, definition.key_columns, definition.null_policy);
    };

    std::set<PageNo> live_pages;
    std::vector<PageNo> dfs_leaf_order;
    std::vector<std::string> dfs_tokens;

    std::function<bool(PageNo, const std::optional<std::vector<char>> &, const std::optional<std::vector<char>> &)> walk;
    walk = [&](PageNo page_no,
               const std::optional<std::vector<char>> &low_key,
               const std::optional<std::vector<char>> &high_key) -> bool {
        if (page_no == INVALID_PAGE_NO) {
            if (error != nullptr) *error = "Encountered invalid child pointer during generic live-tree traversal";
            return false;
        }
        if (!live_pages.insert(page_no).second) {
            if (error != nullptr) *error = "Generic live tree traversal revisited page " + std::to_string(page_no);
            return false;
        }
        PageId pid{rel, page_no};
        Page *page = bpm->FetchPage(pid);
        if (page == nullptr) {
            if (error != nullptr) *error = "Failed to fetch live generic B+ tree page " + std::to_string(page_no);
            return false;
        }
        page->RLatch();
        GenericBTreeNodePage node(page, index.GetMaxKeyBytes(), compare);
        if (node.IsRetired()) {
            page->RUnlatch();
            bpm->UnpinPage(pid, false);
            if (error != nullptr) *error = "Retired generic page " + std::to_string(page_no) + " is reachable from the live root";
            return false;
        }

        if (node.IsLeaf()) {
            std::vector<GenericLeafEntry> entries = node.ReadLeafEntries();
            std::vector<std::vector<char>> keys;
            keys.reserve(entries.size());
            for (const auto &entry : entries) keys.push_back(entry.key);
            if (!CheckNonDecreasingEncoded(keys, compare, error, "Generic leaf", page_no)) {
                page->RUnlatch();
                bpm->UnpinPage(pid, false);
                return false;
            }
            for (const auto &entry : entries) {
                if (low_key.has_value() && compare(entry.key, *low_key) < 0) {
                    page->RUnlatch();
                    bpm->UnpinPage(pid, false);
                    if (error != nullptr) *error = "Generic leaf key fell below its lower fence on page " + std::to_string(page_no);
                    return false;
                }
                if (high_key.has_value() && compare(entry.key, *high_key) >= 0) {
                    page->RUnlatch();
                    bpm->UnpinPage(pid, false);
                    if (error != nullptr) *error = "Generic leaf key crossed its upper fence on page " + std::to_string(page_no);
                    return false;
                }
                dfs_tokens.push_back(EncodedEntryToken(entry.key, entry.rid));
            }
            dfs_leaf_order.push_back(page_no);
            page->RUnlatch();
            bpm->UnpinPage(pid, false);
            return true;
        }

        std::vector<GenericInternalEntry> entries = node.ReadInternalEntries();
        std::vector<std::vector<char>> keys;
        keys.reserve(entries.size());
        for (const auto &entry : entries) keys.push_back(entry.key);
        if (!CheckStrictlySortedEncoded(keys, compare, error, "Generic internal", page_no)) {
            page->RUnlatch();
            bpm->UnpinPage(pid, false);
            return false;
        }

        std::vector<PageNo> children;
        children.reserve(entries.size() + 1);
        children.push_back(node.GetLeftmostChild());
        for (const auto &entry : entries) children.push_back(entry.child);
        page->RUnlatch();
        bpm->UnpinPage(pid, false);

        for (std::size_t i = 0; i < children.size(); ++i) {
            std::optional<std::vector<char>> child_low = (i == 0 ? low_key : std::optional<std::vector<char>>(entries[i - 1].key));
            std::optional<std::vector<char>> child_high = (i == entries.size() ? high_key : std::optional<std::vector<char>>(entries[i].key));
            if (!walk(children[i], child_low, child_high)) return false;
        }
        return true;
    };

    if (!walk(root_page_no, std::nullopt, std::nullopt)) return false;

    std::vector<PageNo> chain_live_leafs;
    PageNo current = root_page_no;
    while (true) {
        PageId pid{rel, current};
        Page *page = bpm->FetchPage(pid);
        if (page == nullptr) {
            if (error != nullptr) *error = "Failed to fetch generic leftmost-descent page";
            return false;
        }
        page->RLatch();
        GenericBTreeNodePage node(page, index.GetMaxKeyBytes(), compare);
        if (node.IsRetired()) {
            PageNo redirect = node.IsLeaf() ? node.GetRightLink() : node.GetLeftmostChild();
            page->RUnlatch();
            bpm->UnpinPage(pid, false);
            if (redirect == INVALID_PAGE_NO) {
                if (error != nullptr) *error = "Retired generic page had no recovery link during leftmost descent";
                return false;
            }
            current = redirect;
            continue;
        }
        if (node.IsLeaf()) {
            page->RUnlatch();
            bpm->UnpinPage(pid, false);
            break;
        }
        current = node.GetLeftmostChild();
        page->RUnlatch();
        bpm->UnpinPage(pid, false);
    }

    std::size_t hop_budget = static_cast<std::size_t>(num_pages) + 4;
    while (current != INVALID_PAGE_NO && hop_budget-- > 0) {
        PageId pid{rel, current};
        Page *page = bpm->FetchPage(pid);
        if (page == nullptr) {
            if (error != nullptr) *error = "Failed to fetch generic leaf-chain page " + std::to_string(current);
            return false;
        }
        page->RLatch();
        GenericBTreeNodePage node(page, index.GetMaxKeyBytes(), compare);
        if (!node.IsLeaf()) {
            page->RUnlatch();
            bpm->UnpinPage(pid, false);
            if (error != nullptr) *error = "Generic leaf chain reached non-leaf page " + std::to_string(current);
            return false;
        }
        PageNo next = node.GetRightLink();
        if (!node.IsRetired()) chain_live_leafs.push_back(current);
        page->RUnlatch();
        bpm->UnpinPage(pid, false);
        current = next;
    }
    if (current != INVALID_PAGE_NO) {
        if (error != nullptr) *error = "Generic leaf chain did not terminate within the page budget";
        return false;
    }
    if (dfs_leaf_order != chain_live_leafs) {
        if (error != nullptr) *error = "Generic leaf chain order does not match DFS leaf order";
        return false;
    }

    std::vector<std::string> scan_tokens;
    for (const auto &entry : index.FullScanEntries()) {
        std::vector<char> encoded = CompositeKeyCodec::EncodeKey(entry.key_values, definition.key_columns, definition.null_policy);
        scan_tokens.push_back(EncodedEntryToken(encoded, entry.rid));
    }
    auto sorted_dfs = dfs_tokens;
    auto sorted_scan = scan_tokens;
    std::sort(sorted_dfs.begin(), sorted_dfs.end());
    std::sort(sorted_scan.begin(), sorted_scan.end());
    if (sorted_dfs != sorted_scan) {
        if (error != nullptr) *error = "Generic ordered scan entries do not match the live leaf contents";
        return false;
    }
    if (std::unordered_set<std::string>(sorted_dfs.begin(), sorted_dfs.end()).size() != sorted_dfs.size()) {
        if (error != nullptr) *error = "Duplicate exact (key,rid) entries found in generic B+ tree";
        return false;
    }

    for (PageNo page_no = 1; page_no < num_pages; ++page_no) {
        PageId pid{rel, page_no};
        Page *page = bpm->FetchPage(pid);
        if (page == nullptr) continue;
        page->RLatch();
        SlottedPage sp(page);
        if (sp.IsInitialized() && (sp.GetPageType() == PageType::BTREE_LEAF || sp.GetPageType() == PageType::BTREE_INTERNAL)) {
            GenericBTreeNodePage node(page, index.GetMaxKeyBytes(), compare);
            if (page_no == root_page_no && node.IsRetired()) {
                page->RUnlatch();
                bpm->UnpinPage(pid, false);
                if (error != nullptr) *error = "Generic meta root points to a retired page";
                return false;
            }
            if (node.IsRetired() && live_pages.count(page_no) != 0) {
                page->RUnlatch();
                bpm->UnpinPage(pid, false);
                if (error != nullptr) *error = "Retired generic page appears in the live-page set";
                return false;
            }
        }
        page->RUnlatch();
        bpm->UnpinPage(pid, false);
    }

    return true;
}

}  // namespace

bool ConsistencyChecker::VerifyHeapReadable(const HeapFile &heap_file, std::string *error) {
    try {
        HeapFileIterator it(&heap_file);
        while (it.HasNext()) {
            auto [rid, tuple] = it.Next();
            (void)rid;
            (void)tuple;
        }
        return true;
    } catch (const std::exception &ex) {
        if (error != nullptr) {
            *error = std::string("Heap scan failed: ") + ex.what();
        }
        return false;
    }
}

bool ConsistencyChecker::VerifyIndexAgainstHeap(const HeapFile &heap_file,
                                                const BTreeIndex &index,
                                                std::size_t key_column_idx,
                                                std::string *error) {
    return VerifyIndexAgainstHeap(heap_file, BTreeIndexAdapter(const_cast<BTreeIndex *>(&index)), {key_column_idx}, error);
}

bool ConsistencyChecker::VerifyIndexAgainstHeap(const HeapFile &heap_file,
                                                const AbstractIndex &index,
                                                const std::vector<std::size_t> &key_column_indexes,
                                                std::string *error) {
    try {
        std::vector<std::string> expected_entries;
        HeapFileIterator heap_it(&heap_file);

        while (heap_it.HasNext()) {
            auto [rid, tuple] = heap_it.Next();
            std::vector<Value> key_values = ExtractKeyValues(tuple, key_column_indexes);

            std::vector<RID> hits = index.SearchExact(key_values);
            if (!ContainsRID(hits, rid)) {
                if (error != nullptr) {
                    *error = "Heap tuple missing from index for RID " + RidToString(rid);
                }
                return false;
            }

            expected_entries.push_back(EntryToken(key_values, rid));
        }

        std::vector<std::string> actual_entries;
        if (const BTreeIndex *btree = index.AsBTreeIndex(); btree != nullptr) {
            BTreeIndexIterator index_it(btree);
            while (index_it.HasNext()) {
                auto [key, rid] = index_it.Next();
                Tuple tuple;
                if (!heap_file.GetTuple(rid, &tuple)) {
                    if (error != nullptr) {
                        *error = "Index points to missing heap RID " + RidToString(rid);
                    }
                    return false;
                }
                std::vector<Value> key_values = ExtractKeyValues(tuple, key_column_indexes);
                if (key_values.size() != 1 || CompareValues(key, key_values.front()) != 0) {
                    if (error != nullptr) {
                        *error = "Index key does not match heap tuple for RID " + RidToString(rid);
                    }
                    return false;
                }
                actual_entries.push_back(EntryToken(key_values, rid));
            }
        } else if (const auto *generic = dynamic_cast<const GenericBTreeIndex *>(&index); generic != nullptr) {
            std::vector<RID> rids = generic->FullScanRids();
            for (const RID &rid : rids) {
                Tuple tuple;
                if (!heap_file.GetTuple(rid, &tuple)) {
                    if (error != nullptr) {
                        *error = "Generic index points to missing heap RID " + RidToString(rid);
                    }
                    return false;
                }
                actual_entries.push_back(EntryToken(ExtractKeyValues(tuple, key_column_indexes), rid));
            }
        } else {
            if (error != nullptr) {
                *error = "Unsupported runtime index type in consistency checker";
            }
            return false;
        }

        std::sort(expected_entries.begin(), expected_entries.end());
        std::sort(actual_entries.begin(), actual_entries.end());

        if (expected_entries != actual_entries) {
            if (error != nullptr) {
                *error = "Heap/index entry sets differ";
            }
            return false;
        }

        return true;
    } catch (const std::exception &ex) {
        if (error != nullptr) {
            *error = std::string("Index verification failed: ") + ex.what();
        }
        return false;
    }
}

bool ConsistencyChecker::VerifyBTreeStructure(const BTreeIndex &index, std::string *error) {
    try {
        return VerifyScalarTreeShape(index, error);
    } catch (const std::exception &ex) {
        if (error != nullptr) *error = std::string("Scalar B+ tree structure verification failed: ") + ex.what();
        return false;
    }
}

bool ConsistencyChecker::VerifyBTreeStructure(const GenericBTreeIndex &index, std::string *error) {
    try {
        return VerifyGenericTreeShape(index, error);
    } catch (const std::exception &ex) {
        if (error != nullptr) *error = std::string("Generic B+ tree structure verification failed: ") + ex.what();
        return false;
    }
}

}  // namespace simpledb
