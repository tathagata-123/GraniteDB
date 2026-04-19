#include "btree_page.h"

#include <cassert>

#include <cstring>
#include <stdexcept>

namespace simpledb {

BTreeNodePage::BTreeNodePage(Page *page, TypeId key_type, uint32_t max_varchar_len)
    : page_(page), key_type_(key_type), max_varchar_len_(max_varchar_len),
      max_key_bytes_(IndexKeyUtil::MaxEncodedKeySize(key_type, max_varchar_len)) {}

uint16_t BTreeNodePage::GetSpecialSize() const {
    return static_cast<uint16_t>(sizeof(BTreeNodeSpecialData) + max_key_bytes_);
}

uint32_t BTreeNodePage::GetMaxKeyBytes() const { return max_key_bytes_; }

BTreeNodeSpecialData &BTreeNodePage::Special() {
    SlottedPage sp(page_);
    return *reinterpret_cast<BTreeNodeSpecialData *>(page_->GetData() + sp.GetSpecialOffset());
}

const BTreeNodeSpecialData &BTreeNodePage::Special() const {
    SlottedPage sp(const_cast<Page *>(page_));
    return *reinterpret_cast<const BTreeNodeSpecialData *>(page_->GetData() + sp.GetSpecialOffset());
}

void BTreeNodePage::InitializeLeaf() {
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

void BTreeNodePage::InitializeInternal(PageNo leftmost_child) {
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

bool BTreeNodePage::IsLeaf() const {
    SlottedPage sp(const_cast<Page *>(page_));
    return sp.IsInitialized() && sp.GetPageType() == PageType::BTREE_LEAF;
}

bool BTreeNodePage::IsInternal() const {
    SlottedPage sp(const_cast<Page *>(page_));
    return sp.IsInitialized() && sp.GetPageType() == PageType::BTREE_INTERNAL;
}

BTreePageState BTreeNodePage::GetPageState() const {
    return static_cast<BTreePageState>(Special().page_state);
}

void BTreeNodePage::SetPageState(BTreePageState state) {
    assert(state == BTreePageState::LIVE || state == BTreePageState::RETIRING || state == BTreePageState::RETIRED);
    Special().page_state = static_cast<uint16_t>(state);
}

bool BTreeNodePage::IsRetired() const { return GetPageState() == BTreePageState::RETIRED; }
bool BTreeNodePage::IsLive() const { return GetPageState() == BTreePageState::LIVE; }

PageNo BTreeNodePage::GetRightLink() const { return Special().right_link; }
void BTreeNodePage::SetRightLink(PageNo page_no) { Special().right_link = page_no; }
PageNo BTreeNodePage::GetLeftmostChild() const { return Special().leftmost_child; }
void BTreeNodePage::SetLeftmostChild(PageNo page_no) { Special().leftmost_child = page_no; }
bool BTreeNodePage::HasHighKey() const { return Special().has_high_key != 0; }

std::vector<char> BTreeNodePage::GetHighKey() const {
    const auto &special = Special();
    if (!special.has_high_key) return {};
    if (special.high_key_len > max_key_bytes_) throw std::runtime_error("Corrupt high key length");
    const char *storage = reinterpret_cast<const char *>(&special + 1);
    return std::vector<char>(storage, storage + special.high_key_len);
}

void BTreeNodePage::SetHighKey(const std::vector<char> *key_or_null) {
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

std::vector<char> BTreeNodePage::EncodeLeafEntry(const LeafEntry &entry) const {
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

LeafEntry BTreeNodePage::DecodeLeafEntry(const char *data, uint16_t len) const {
    if (len < sizeof(uint32_t) + sizeof(PageNo) + sizeof(SlotNo)) throw std::runtime_error("Corrupt leaf entry");
    std::size_t pos = 0;
    uint32_t key_len;
    std::memcpy(&key_len, data + pos, sizeof(key_len)); pos += sizeof(key_len);
    if (key_len > max_key_bytes_) throw std::runtime_error("Leaf entry key too large");
    if (sizeof(uint32_t) + key_len + sizeof(PageNo) + sizeof(SlotNo) != len) throw std::runtime_error("Corrupt leaf entry length");
    LeafEntry entry;
    entry.key.assign(data + pos, data + pos + key_len); pos += key_len;
    std::memcpy(&entry.rid.page_no, data + pos, sizeof(PageNo)); pos += sizeof(PageNo);
    std::memcpy(&entry.rid.slot_no, data + pos, sizeof(SlotNo));
    return entry;
}

std::vector<char> BTreeNodePage::EncodeInternalEntry(const InternalEntry &entry) const {
    if (entry.key.size() > max_key_bytes_) throw std::runtime_error("Internal key too large");
    uint32_t key_len = static_cast<uint32_t>(entry.key.size());
    std::vector<char> out(sizeof(key_len) + key_len + sizeof(PageNo));
    std::size_t pos = 0;
    std::memcpy(out.data() + pos, &key_len, sizeof(key_len)); pos += sizeof(key_len);
    std::memcpy(out.data() + pos, entry.key.data(), key_len); pos += key_len;
    std::memcpy(out.data() + pos, &entry.child, sizeof(PageNo));
    return out;
}

InternalEntry BTreeNodePage::DecodeInternalEntry(const char *data, uint16_t len) const {
    if (len < sizeof(uint32_t) + sizeof(PageNo)) throw std::runtime_error("Corrupt internal entry");
    std::size_t pos = 0;
    uint32_t key_len;
    std::memcpy(&key_len, data + pos, sizeof(key_len)); pos += sizeof(key_len);
    if (key_len > max_key_bytes_) throw std::runtime_error("Internal entry key too large");
    if (sizeof(uint32_t) + key_len + sizeof(PageNo) != len) throw std::runtime_error("Corrupt internal entry length");
    InternalEntry entry;
    entry.key.assign(data + pos, data + pos + key_len); pos += key_len;
    std::memcpy(&entry.child, data + pos, sizeof(PageNo));
    return entry;
}

bool BTreeNodePage::LeafEntryLess(TypeId key_type, const LeafEntry &a, const LeafEntry &b) {
    int cmp = IndexKeyUtil::CompareEncoded(key_type, a.key, b.key);
    if (cmp != 0) return cmp < 0;
    if (a.rid.page_no != b.rid.page_no) return a.rid.page_no < b.rid.page_no;
    return a.rid.slot_no < b.rid.slot_no;
}

bool BTreeNodePage::InternalEntryLess(TypeId key_type, const InternalEntry &a, const InternalEntry &b) {
    return IndexKeyUtil::CompareEncoded(key_type, a.key, b.key) < 0;
}

std::vector<LeafEntry> BTreeNodePage::ReadLeafEntries() const {
    if (!IsLeaf()) throw std::runtime_error("ReadLeafEntries called on non-leaf");
    SlottedPage sp(const_cast<Page *>(page_));
    std::vector<LeafEntry> entries;
    for (SlotNo i = 0; i < sp.GetSlotCount(); i++) {
        const char *raw = nullptr; uint16_t raw_len = 0;
        if (sp.GetRecord(i, &raw, &raw_len)) entries.push_back(DecodeLeafEntry(raw, raw_len));
    }
    std::sort(entries.begin(), entries.end(), [&](const LeafEntry &a, const LeafEntry &b){ return LeafEntryLess(key_type_, a, b); });
    return entries;
}

std::vector<InternalEntry> BTreeNodePage::ReadInternalEntries() const {
    if (!IsInternal()) throw std::runtime_error("ReadInternalEntries called on non-internal");
    SlottedPage sp(const_cast<Page *>(page_));
    std::vector<InternalEntry> entries;
    for (SlotNo i = 0; i < sp.GetSlotCount(); i++) {
        const char *raw = nullptr; uint16_t raw_len = 0;
        if (sp.GetRecord(i, &raw, &raw_len)) entries.push_back(DecodeInternalEntry(raw, raw_len));
    }
    std::sort(entries.begin(), entries.end(), [&](const InternalEntry &a, const InternalEntry &b){ return InternalEntryLess(key_type_, a, b); });
    return entries;
}

bool BTreeNodePage::RewriteLeaf(const std::vector<LeafEntry> &entries, PageNo right_link, const std::vector<char> *high_key_or_null) {
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

bool BTreeNodePage::RewriteInternal(PageNo leftmost_child, const std::vector<InternalEntry> &entries, PageNo right_link, const std::vector<char> *high_key_or_null) {
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

PageNo BTreeNodePage::FindChildForKey(const std::vector<char> &search_key) const {
    if (!IsInternal()) throw std::runtime_error("FindChildForKey called on non-internal");
    std::vector<InternalEntry> entries = ReadInternalEntries();
    PageNo child = GetLeftmostChild();
    for (const auto &entry : entries) {
        int cmp = IndexKeyUtil::CompareEncoded(key_type_, search_key, entry.key);
        if (cmp < 0) break;
        child = entry.child;
    }
    return child;
}

}  // namespace simpledb
