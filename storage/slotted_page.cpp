#include "slotted_page.h"

#include <cstring>
#include <stdexcept>

namespace simpledb {

SlottedPage::SlottedPage(Page *page) : page_(page) {}

SlottedPageHeader &SlottedPage::Header() {
    return *reinterpret_cast<SlottedPageHeader *>(page_->GetData());
}

const SlottedPageHeader &SlottedPage::Header() const {
    return *reinterpret_cast<const SlottedPageHeader *>(page_->GetData());
}

SlotEntry &SlottedPage::SlotAt(SlotNo slot_no) {
    char *base = page_->GetData() + sizeof(SlottedPageHeader);
    return reinterpret_cast<SlotEntry *>(base)[slot_no];
}

const SlotEntry &SlottedPage::SlotAt(SlotNo slot_no) const {
    const char *base = page_->GetData() + sizeof(SlottedPageHeader);
    return reinterpret_cast<const SlotEntry *>(base)[slot_no];
}

bool SlottedPage::SlotExists(SlotNo slot_no) const {
    return slot_no < Header().slot_count;
}

void SlottedPage::Initialize(PageType page_type, uint16_t special_size) {
    if (special_size > PAGE_SIZE - sizeof(SlottedPageHeader)) {
        throw std::runtime_error("special_size too large for page");
    }

    std::memset(page_->GetData(), 0, PAGE_SIZE);

    Header().page_lsn = 0;
    Header().lower = static_cast<uint16_t>(sizeof(SlottedPageHeader));
    Header().special = static_cast<uint16_t>(PAGE_SIZE - special_size);
    Header().upper = Header().special;
    Header().slot_count = 0;
    Header().page_type = page_type;
    Header().flags = 0;
    Header().reserved = 0;
}

bool SlottedPage::IsInitialized() const {
    const auto &hdr = Header();
    if (hdr.page_type == PageType::INVALID) {
        return false;
    }
    if (hdr.lower < sizeof(SlottedPageHeader)) {
        return false;
    }
    if (hdr.lower > hdr.upper || hdr.upper > hdr.special || hdr.special > PAGE_SIZE) {
        return false;
    }
    return true;
}

PageType SlottedPage::GetPageType() const { return Header().page_type; }
uint16_t SlottedPage::GetSlotCount() const { return Header().slot_count; }
uint16_t SlottedPage::GetSpecialOffset() const { return Header().special; }

uint16_t SlottedPage::GetContiguousFreeSpace() const {
    if (Header().upper < Header().lower) {
        throw std::runtime_error("Corrupt slotted page");
    }
    return static_cast<uint16_t>(Header().upper - Header().lower);
}

uint16_t SlottedPage::GetReclaimableSpace() const {
    uint32_t reclaimable = 0;
    for (SlotNo i = 0; i < Header().slot_count; i++) {
        const SlotEntry &slot = SlotAt(i);
        if (slot.state == SlotState::DELETED) {
            reclaimable += slot.length;
        }
    }
    return static_cast<uint16_t>(reclaimable);
}

uint16_t SlottedPage::GetTotalFreeSpaceAfterCompaction() const {
    return static_cast<uint16_t>(GetContiguousFreeSpace() + GetReclaimableSpace());
}

bool SlottedPage::HasReusableSlot() const { return FindReusableSlot() != -1; }

uint16_t SlottedPage::GetMaxInsertableBytesAfterCompaction() const {
    uint16_t total_free = GetTotalFreeSpaceAfterCompaction();
    uint16_t slot_overhead = HasReusableSlot() ? 0 : static_cast<uint16_t>(sizeof(SlotEntry));
    if (total_free <= slot_overhead) return 0;
    return static_cast<uint16_t>(total_free - slot_overhead);
}

int SlottedPage::FindReusableSlot() const {
    for (SlotNo i = 0; i < Header().slot_count; i++) {
        const SlotEntry &slot = SlotAt(i);
        if (slot.state == SlotState::EMPTY || slot.state == SlotState::DELETED) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool SlottedPage::HasSpaceFor(uint16_t record_len, bool needs_new_slot) const {
    uint32_t required = record_len + (needs_new_slot ? sizeof(SlotEntry) : 0);
    return GetContiguousFreeSpace() >= required;
}

bool SlottedPage::InsertRecord(const char *data, uint16_t len, SlotNo *out_slot_no) {
    if (len == 0) return false;

    int reusable_slot = FindReusableSlot();
    bool needs_new_slot = (reusable_slot == -1);

    if (!HasSpaceFor(len, needs_new_slot)) {
        uint32_t required = len + (needs_new_slot ? sizeof(SlotEntry) : 0);
        if (GetTotalFreeSpaceAfterCompaction() < required) {
            return false;
        }
        CompactPage();
        if (!HasSpaceFor(len, needs_new_slot)) {
            return false;
        }
    }

    SlotNo slot_no;
    if (reusable_slot != -1) {
        slot_no = static_cast<SlotNo>(reusable_slot);
    } else {
        slot_no = Header().slot_count;
        Header().slot_count++;
        Header().lower = static_cast<uint16_t>(Header().lower + sizeof(SlotEntry));
    }

    Header().upper = static_cast<uint16_t>(Header().upper - len);
    uint16_t offset = Header().upper;
    std::memcpy(page_->GetData() + offset, data, len);

    SlotEntry &slot = SlotAt(slot_no);
    slot.offset = offset;
    slot.length = len;
    slot.state = SlotState::OCCUPIED;
    slot.reserved = 0;

    if (out_slot_no != nullptr) *out_slot_no = slot_no;
    return true;
}

bool SlottedPage::GetRecord(SlotNo slot_no, const char **out_data, uint16_t *out_len) const {
    if (!SlotExists(slot_no)) return false;
    const SlotEntry &slot = SlotAt(slot_no);
    if (slot.state != SlotState::OCCUPIED) return false;
    if (slot.offset + slot.length > Header().special) {
        throw std::runtime_error("Corrupt slot entry");
    }
    if (out_data) *out_data = page_->GetData() + slot.offset;
    if (out_len) *out_len = slot.length;
    return true;
}

bool SlottedPage::GetMutableRecord(SlotNo slot_no, char **out_data, uint16_t *out_len) {
    if (!SlotExists(slot_no)) return false;
    SlotEntry &slot = SlotAt(slot_no);
    if (slot.state != SlotState::OCCUPIED) return false;
    if (slot.offset + slot.length > Header().special) {
        throw std::runtime_error("Corrupt slot entry");
    }
    if (out_data) *out_data = page_->GetData() + slot.offset;
    if (out_len) *out_len = slot.length;
    return true;
}

bool SlottedPage::OverwriteRecord(SlotNo slot_no, const char *data, uint16_t len) {
    if (!SlotExists(slot_no)) return false;
    SlotEntry &slot = SlotAt(slot_no);
    if (slot.state != SlotState::OCCUPIED) return false;
    if (len > slot.length) return false;
    std::memcpy(page_->GetData() + slot.offset, data, len);
    if (len < slot.length) {
        std::memset(page_->GetData() + slot.offset + len, 0, slot.length - len);
    }
    return true;
}

bool SlottedPage::DeleteRecord(SlotNo slot_no) {
    if (!SlotExists(slot_no)) return false;
    SlotEntry &slot = SlotAt(slot_no);
    if (slot.state != SlotState::OCCUPIED) return false;
    slot.state = SlotState::DELETED;
    return true;
}

bool SlottedPage::IsSlotOccupied(SlotNo slot_no) const {
    return SlotExists(slot_no) && SlotAt(slot_no).state == SlotState::OCCUPIED;
}

bool SlottedPage::IsSlotDeleted(SlotNo slot_no) const {
    return SlotExists(slot_no) && SlotAt(slot_no).state == SlotState::DELETED;
}

bool SlottedPage::IsEmpty() const {
    for (SlotNo i = 0; i < Header().slot_count; i++) {
        if (SlotAt(i).state == SlotState::OCCUPIED) {
            return false;
        }
    }
    return true;
}

void SlottedPage::CompactPage() {
    struct LiveRecord {
        SlotNo slot_no;
        std::vector<char> bytes;
    };

    std::vector<LiveRecord> live_records;
    live_records.reserve(Header().slot_count);

    for (SlotNo i = 0; i < Header().slot_count; i++) {
        SlotEntry &slot = SlotAt(i);
        if (slot.state == SlotState::OCCUPIED) {
            if (slot.offset + slot.length > Header().special) {
                throw std::runtime_error("Corrupt slot during compaction");
            }
            LiveRecord rec;
            rec.slot_no = i;
            rec.bytes.resize(slot.length);
            std::memcpy(rec.bytes.data(), page_->GetData() + slot.offset, slot.length);
            live_records.push_back(std::move(rec));
        } else {
            slot.offset = 0;
            slot.length = 0;
            slot.state = (slot.state == SlotState::DELETED) ? SlotState::DELETED : SlotState::EMPTY;
        }
    }

    std::memset(page_->GetData() + Header().lower, 0, Header().special - Header().lower);

    uint16_t new_upper = Header().special;
    for (const auto &rec : live_records) {
        new_upper = static_cast<uint16_t>(new_upper - rec.bytes.size());
        std::memcpy(page_->GetData() + new_upper, rec.bytes.data(), rec.bytes.size());

        SlotEntry &slot = SlotAt(rec.slot_no);
        slot.offset = new_upper;
        slot.length = static_cast<uint16_t>(rec.bytes.size());
        slot.state = SlotState::OCCUPIED;
    }

    Header().upper = new_upper;
}

}  // namespace simpledb
