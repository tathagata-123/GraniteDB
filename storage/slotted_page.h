#pragma once

#include <cstdint>
#include <vector>

#include "page.h"

namespace simpledb {

enum class PageType : uint16_t {
    INVALID = 0,
    HEAP = 1,
    BTREE_LEAF = 2,
    BTREE_INTERNAL = 3
};

enum class SlotState : uint16_t {
    EMPTY = 0,
    OCCUPIED = 1,
    DELETED = 2
};

struct SlottedPageHeader {
    LSN page_lsn;
    uint16_t lower;
    uint16_t upper;
    uint16_t special;
    uint16_t slot_count;
    PageType page_type;
    uint16_t flags;
    uint16_t reserved;
};

struct SlotEntry {
    uint16_t offset;
    uint16_t length;
    SlotState state;
    uint16_t reserved;
};

class SlottedPage {
public:
    explicit SlottedPage(Page *page);

    void Initialize(PageType page_type, uint16_t special_size = 0);
    bool IsInitialized() const;

    PageType GetPageType() const;
    uint16_t GetSlotCount() const;
    uint16_t GetSpecialOffset() const;

    uint16_t GetContiguousFreeSpace() const;
    uint16_t GetReclaimableSpace() const;
    uint16_t GetTotalFreeSpaceAfterCompaction() const;

    bool HasReusableSlot() const;
    uint16_t GetMaxInsertableBytesAfterCompaction() const;

    bool InsertRecord(const char *data, uint16_t len, SlotNo *out_slot_no);

    bool GetRecord(SlotNo slot_no, const char **out_data, uint16_t *out_len) const;
    bool GetMutableRecord(SlotNo slot_no, char **out_data, uint16_t *out_len);

    bool OverwriteRecord(SlotNo slot_no, const char *data, uint16_t len);
    bool DeleteRecord(SlotNo slot_no);

    bool IsSlotOccupied(SlotNo slot_no) const;
    bool IsSlotDeleted(SlotNo slot_no) const;
    bool IsEmpty() const;

    void CompactPage();

private:
    Page *page_;

    SlottedPageHeader &Header();
    const SlottedPageHeader &Header() const;

    SlotEntry &SlotAt(SlotNo slot_no);
    const SlotEntry &SlotAt(SlotNo slot_no) const;

    bool SlotExists(SlotNo slot_no) const;
    bool HasSpaceFor(uint16_t record_len, bool needs_new_slot) const;
    int FindReusableSlot() const;
};

}  // namespace simpledb
