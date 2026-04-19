#include "generic_btree_page.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace simpledb {

GenericBTreeNodePage::GenericBTreeNodePage(Page *page,
                                           uint32_t max_key_bytes,
                                           KeyComparator comparator)
    : page_(page), max_key_bytes_(max_key_bytes), comparator_(std::move(comparator)) {
    if (!comparator_) {
        throw std::runtime_error("GenericBTreeNodePage requires a comparator");
    }
}

uint16_t GenericBTreeNodePage::GetSpecialSize() const {
    return static_cast<uint16_t>(sizeof(BTreeNodeSpecialData) + max_key_bytes_);
}

BTreeNodeSpecialData &GenericBTreeNodePage::Special() {
    SlottedPage sp(page_);
    return *reinterpret_cast<BTreeNodeSpecialData *>(page_->GetData() + sp.GetSpecialOffset());
}

const BTreeNodeSpecialData &GenericBTreeNodePage::Special() const {
    SlottedPage sp(const_cast<Page *>(page_));
    return *reinterpret_cast<const BTreeNodeSpecialData *>(page_->GetData() + sp.GetSpecialOffset());
}

void GenericBTreeNodePage::InitializeLeaf() {
    SlottedPage sp(page_);
    sp.Initialize(PageType::BTREE_LEAF, GetSpecialSize());
    auto &special = Special();
    std::memset(&special, 0, sizeof(BTreeNodeSpecialData) + max_key_bytes_);
    special.magic = BTREE_MAGIC;
    special.is_leaf = 1;
    special.right_link = INVALID_PAGE_NO;
    special.leftmost_child = INVALID_PAGE_NO;
    special.page_state = static_cast<uint16_t>(BTreePageState::LIVE);
    special.has_high_key = 0;
    special.high_key_len = 0;
}

void GenericBTreeNodePage::InitializeInternal(PageNo leftmost_child) {
    SlottedPage sp(page_);
    sp.Initialize(PageType::BTREE_INTERNAL, GetSpecialSize());
    auto &special = Special();
    std::memset(&special, 0, sizeof(BTreeNodeSpecialData) + max_key_bytes_);
    special.magic = BTREE_MAGIC;
    special.is_leaf = 0;
    special.right_link = INVALID_PAGE_NO;
    special.leftmost_child = leftmost_child;
    special.page_state = static_cast<uint16_t>(BTreePageState::LIVE);
    special.has_high_key = 0;
    special.high_key_len = 0;
}

bool GenericBTreeNodePage::IsLeaf() const {
    SlottedPage sp(const_cast<Page *>(page_));
    return sp.IsInitialized() && sp.GetPageType() == PageType::BTREE_LEAF;
}

bool GenericBTreeNodePage::IsInternal() const {
    SlottedPage sp(const_cast<Page *>(page_));
    return sp.IsInitialized() && sp.GetPageType() == PageType::BTREE_INTERNAL;
}

BTreePageState GenericBTreeNodePage::GetPageState() const {
    return static_cast<BTreePageState>(Special().page_state);
}

void GenericBTreeNodePage::SetPageState(BTreePageState state) {
    assert(state == BTreePageState::LIVE || state == BTreePageState::RETIRING || state == BTreePageState::RETIRED);
    Special().page_state = static_cast<uint16_t>(state);
}

bool GenericBTreeNodePage::IsRetired() const { return GetPageState() == BTreePageState::RETIRED; }
bool GenericBTreeNodePage::IsLive() const { return GetPageState() == BTreePageState::LIVE; }

PageNo GenericBTreeNodePage::GetRightLink() const { return Special().right_link; }
void GenericBTreeNodePage::SetRightLink(PageNo page_no) { Special().right_link = page_no; }
PageNo GenericBTreeNodePage::GetLeftmostChild() const { return Special().leftmost_child; }
void GenericBTreeNodePage::SetLeftmostChild(PageNo page_no) { Special().leftmost_child = page_no; }
bool GenericBTreeNodePage::HasHighKey() const { return Special().has_high_key != 0; }

std::vector<char> GenericBTreeNodePage::GetHighKey() const {
    const auto &special = Special();
    if (!special.has_high_key) return {};
    if (special.high_key_len > max_key_bytes_) throw std::runtime_error("Corrupt high key length");
    const char *storage = reinterpret_cast<const char *>(&special + 1);
    return std::vector<char>(storage, storage + special.high_key_len);
}

void GenericBTreeNodePage::SetHighKey(const std::vector<char> *key_or_null) {
    auto &special = Special();
    char *storage = reinterpret_cast<char *>(&special + 1);
    if (key_or_null == nullptr) {
        special.has_high_key = 0;
        special.high_key_len = 0;
        std::memset(storage, 0, max_key_bytes_);
        return;
    }
    if (key_or_null->size() > max_key_bytes_) throw std::runtime_error("High key too large for node metadata area");
    special.has_high_key = 1;
    special.high_key_len = static_cast<uint32_t>(key_or_null->size());
    std::memset(storage, 0, max_key_bytes_);
    std::memcpy(storage, key_or_null->data(), key_or_null->size());
}

std::vector<char> GenericBTreeNodePage::EncodeLeafEntry(const GenericLeafEntry &entry) const {
    if (entry.key.size() > max_key_bytes_) throw std::runtime_error("Leaf key too large");
    uint32_t key_len = static_cast<uint32_t>(entry.key.size());
    std::vector<char> out(sizeof(key_len) + key_len + sizeof(PageNo) + sizeof(SlotNo));
    std::size_t pos = 0;
    std::memcpy(out.data() + pos, &key_len, sizeof(key_len)); pos += sizeof(key_len);
    std::memcpy(out.data() + pos, entry.key.data(), key_len); pos += key_len;
    std::memcpy(out.data() + pos, &entry.rid.page_no, sizeof(PageNo)); pos += sizeof(PageNo);
    std::memcpy(out.data() + pos, &entry.rid.slot_no, sizeof(SlotNo));
    return out;
}

GenericLeafEntry GenericBTreeNodePage::DecodeLeafEntry(const char *data, uint16_t len) const {
    if (len < sizeof(uint32_t) + sizeof(PageNo) + sizeof(SlotNo)) throw std::runtime_error("Corrupt leaf entry");
    std::size_t pos = 0;
    uint32_t key_len;
    std::memcpy(&key_len, data + pos, sizeof(key_len)); pos += sizeof(key_len);
    if (key_len > max_key_bytes_) throw std::runtime_error("Leaf entry key too large");
    if (sizeof(uint32_t) + key_len + sizeof(PageNo) + sizeof(SlotNo) != len) throw std::runtime_error("Corrupt leaf entry length");
    GenericLeafEntry entry;
    entry.key.assign(data + pos, data + pos + key_len); pos += key_len;
    std::memcpy(&entry.rid.page_no, data + pos, sizeof(PageNo)); pos += sizeof(PageNo);
    std::memcpy(&entry.rid.slot_no, data + pos, sizeof(SlotNo));
    return entry;
}

std::vector<char> GenericBTreeNodePage::EncodeInternalEntry(const GenericInternalEntry &entry) const {
    if (entry.key.size() > max_key_bytes_) throw std::runtime_error("Internal key too large");
    uint32_t key_len = static_cast<uint32_t>(entry.key.size());
    std::vector<char> out(sizeof(key_len) + key_len + sizeof(PageNo));
    std::size_t pos = 0;
    std::memcpy(out.data() + pos, &key_len, sizeof(key_len)); pos += sizeof(key_len);
    std::memcpy(out.data() + pos, entry.key.data(), key_len); pos += key_len;
    std::memcpy(out.data() + pos, &entry.child, sizeof(PageNo));
    return out;
}

GenericInternalEntry GenericBTreeNodePage::DecodeInternalEntry(const char *data, uint16_t len) const {
    if (len < sizeof(uint32_t) + sizeof(PageNo)) throw std::runtime_error("Corrupt internal entry");
    std::size_t pos = 0;
    uint32_t key_len;
    std::memcpy(&key_len, data + pos, sizeof(key_len)); pos += sizeof(key_len);
    if (key_len > max_key_bytes_) throw std::runtime_error("Internal entry key too large");
    if (sizeof(uint32_t) + key_len + sizeof(PageNo) != len) throw std::runtime_error("Corrupt internal entry length");
    GenericInternalEntry entry;
    entry.key.assign(data + pos, data + pos + key_len); pos += key_len;
    std::memcpy(&entry.child, data + pos, sizeof(PageNo));
    return entry;
}

std::vector<GenericLeafEntry> GenericBTreeNodePage::ReadLeafEntries() const {
    if (!IsLeaf()) throw std::runtime_error("ReadLeafEntries called on non-leaf");
    SlottedPage sp(const_cast<Page *>(page_));
    std::vector<GenericLeafEntry> entries;
    for (SlotNo i = 0; i < sp.GetSlotCount(); i++) {
        const char *raw = nullptr; uint16_t raw_len = 0;
        if (sp.GetRecord(i, &raw, &raw_len)) entries.push_back(DecodeLeafEntry(raw, raw_len));
    }
    std::sort(entries.begin(), entries.end(), [&](const GenericLeafEntry &a, const GenericLeafEntry &b) {
        int cmp = comparator_(a.key, b.key);
        if (cmp != 0) return cmp < 0;
        if (a.rid.page_no != b.rid.page_no) return a.rid.page_no < b.rid.page_no;
        return a.rid.slot_no < b.rid.slot_no;
    });
    return entries;
}

std::vector<GenericInternalEntry> GenericBTreeNodePage::ReadInternalEntries() const {
    if (!IsInternal()) throw std::runtime_error("ReadInternalEntries called on non-internal");
    SlottedPage sp(const_cast<Page *>(page_));
    std::vector<GenericInternalEntry> entries;
    for (SlotNo i = 0; i < sp.GetSlotCount(); i++) {
        const char *raw = nullptr; uint16_t raw_len = 0;
        if (sp.GetRecord(i, &raw, &raw_len)) entries.push_back(DecodeInternalEntry(raw, raw_len));
    }
    std::sort(entries.begin(), entries.end(), [&](const GenericInternalEntry &a, const GenericInternalEntry &b) {
        return comparator_(a.key, b.key) < 0;
    });
    return entries;
}

bool GenericBTreeNodePage::RewriteLeaf(const std::vector<GenericLeafEntry> &entries,
                                       PageNo right_link,
                                       const std::vector<char> *high_key_or_null) {
    InitializeLeaf();
    SetRightLink(right_link);
    SetHighKey(high_key_or_null);
    SlottedPage sp(page_);
    for (const auto &entry : entries) {
        std::vector<char> bytes = EncodeLeafEntry(entry);
        SlotNo slot_no = 0;
        if (!sp.InsertRecord(bytes.data(), static_cast<uint16_t>(bytes.size()), &slot_no)) return false;
    }
    return true;
}

bool GenericBTreeNodePage::RewriteInternal(PageNo leftmost_child,
                                           const std::vector<GenericInternalEntry> &entries,
                                           PageNo right_link,
                                           const std::vector<char> *high_key_or_null) {
    InitializeInternal(leftmost_child);
    SetRightLink(right_link);
    SetHighKey(high_key_or_null);
    SlottedPage sp(page_);
    for (const auto &entry : entries) {
        std::vector<char> bytes = EncodeInternalEntry(entry);
        SlotNo slot_no = 0;
        if (!sp.InsertRecord(bytes.data(), static_cast<uint16_t>(bytes.size()), &slot_no)) return false;
    }
    return true;
}

PageNo GenericBTreeNodePage::FindChildForKey(const std::vector<char> &search_key) const {
    if (!IsInternal()) throw std::runtime_error("FindChildForKey called on non-internal");
    std::vector<GenericInternalEntry> entries = ReadInternalEntries();
    PageNo child = GetLeftmostChild();
    for (const auto &entry : entries) {
        int cmp = comparator_(search_key, entry.key);
        if (cmp < 0) break;
        child = entry.child;
    }
    return child;
}

}  // namespace simpledb
