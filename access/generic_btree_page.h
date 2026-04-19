#pragma once

#include <functional>
#include <vector>

#include "btree_page.h"

namespace simpledb {

struct GenericLeafEntry {
    std::vector<char> key;
    RID rid;
};

struct GenericInternalEntry {
    std::vector<char> key;
    PageNo child;
};

class GenericBTreeNodePage {
public:
    using KeyComparator = std::function<int(const std::vector<char> &, const std::vector<char> &)>;

    GenericBTreeNodePage(Page *page, uint32_t max_key_bytes, KeyComparator comparator);

    uint16_t GetSpecialSize() const;
    uint32_t GetMaxKeyBytes() const { return max_key_bytes_; }

    void InitializeLeaf();
    void InitializeInternal(PageNo leftmost_child);

    bool IsLeaf() const;
    bool IsInternal() const;

    BTreePageState GetPageState() const;
    void SetPageState(BTreePageState state);
    bool IsRetired() const;
    bool IsLive() const;

    PageNo GetRightLink() const;
    void SetRightLink(PageNo page_no);

    PageNo GetLeftmostChild() const;
    void SetLeftmostChild(PageNo page_no);

    bool HasHighKey() const;
    std::vector<char> GetHighKey() const;
    void SetHighKey(const std::vector<char> *key_or_null);

    std::vector<GenericLeafEntry> ReadLeafEntries() const;
    std::vector<GenericInternalEntry> ReadInternalEntries() const;

    bool RewriteLeaf(const std::vector<GenericLeafEntry> &entries,
                     PageNo right_link,
                     const std::vector<char> *high_key_or_null);

    bool RewriteInternal(PageNo leftmost_child,
                         const std::vector<GenericInternalEntry> &entries,
                         PageNo right_link,
                         const std::vector<char> *high_key_or_null);

    PageNo FindChildForKey(const std::vector<char> &search_key) const;

private:
    BTreeNodeSpecialData &Special();
    const BTreeNodeSpecialData &Special() const;

    std::vector<char> EncodeLeafEntry(const GenericLeafEntry &entry) const;
    GenericLeafEntry DecodeLeafEntry(const char *data, uint16_t len) const;

    std::vector<char> EncodeInternalEntry(const GenericInternalEntry &entry) const;
    GenericInternalEntry DecodeInternalEntry(const char *data, uint16_t len) const;

private:
    Page *page_;
    uint32_t max_key_bytes_;
    KeyComparator comparator_;
};

}  // namespace simpledb
