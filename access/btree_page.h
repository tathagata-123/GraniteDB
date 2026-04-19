#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "../common/types.h"
#include "../common/value.h"
#include "../storage/page.h"
#include "../storage/slotted_page.h"
#include "index_key.h"

namespace simpledb {

constexpr uint32_t BTREE_MAGIC = 0xBEE70001u;
constexpr PageNo INVALID_PAGE_NO = std::numeric_limits<PageNo>::max();

enum class BTreePageState : uint16_t {
    LIVE = 1,
    RETIRING = 2,
    RETIRED = 3,
};

struct BTreeMetaPageData {
    LSN page_lsn;
    uint32_t magic;
    PageNo root_page_no;
    uint32_t tree_height;
    TypeId key_type;
    uint32_t max_varchar_len;
    uint32_t max_key_bytes;
    uint32_t reserved;
};

struct BTreeNodeSpecialData {
    uint32_t magic;
    uint16_t is_leaf;
    uint16_t page_state;
    PageNo right_link;
    PageNo leftmost_child;
    uint8_t has_high_key;
    uint8_t reserved1[3];
    uint32_t high_key_len;
};

struct LeafEntry {
    std::vector<char> key;
    RID rid;
};

struct InternalEntry {
    std::vector<char> key;
    PageNo child;
};

class BTreeNodePage {
public:
    BTreeNodePage(Page *page, TypeId key_type, uint32_t max_varchar_len);

    uint16_t GetSpecialSize() const;
    uint32_t GetMaxKeyBytes() const;

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

    std::vector<LeafEntry> ReadLeafEntries() const;
    std::vector<InternalEntry> ReadInternalEntries() const;

    bool RewriteLeaf(const std::vector<LeafEntry> &entries,
                     PageNo right_link,
                     const std::vector<char> *high_key_or_null);

    bool RewriteInternal(PageNo leftmost_child,
                         const std::vector<InternalEntry> &entries,
                         PageNo right_link,
                         const std::vector<char> *high_key_or_null);

    PageNo FindChildForKey(const std::vector<char> &search_key) const;

private:
    BTreeNodeSpecialData &Special();
    const BTreeNodeSpecialData &Special() const;

    std::vector<char> EncodeLeafEntry(const LeafEntry &entry) const;
    LeafEntry DecodeLeafEntry(const char *data, uint16_t len) const;

    std::vector<char> EncodeInternalEntry(const InternalEntry &entry) const;
    InternalEntry DecodeInternalEntry(const char *data, uint16_t len) const;

    static bool LeafEntryLess(TypeId key_type, const LeafEntry &a, const LeafEntry &b);
    static bool InternalEntryLess(TypeId key_type, const InternalEntry &a, const InternalEntry &b);

private:
    Page *page_;
    TypeId key_type_;
    uint32_t max_varchar_len_;
    uint32_t max_key_bytes_;
};

}  // namespace simpledb
