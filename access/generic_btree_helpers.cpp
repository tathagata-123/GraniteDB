#include "generic_btree_helpers.h"

namespace simpledb::generic_btree_helpers {

uint32_t LeafEntryBytes(const GenericLeafEntry &entry) {
    return static_cast<uint32_t>(sizeof(uint32_t) + entry.key.size() + sizeof(PageNo) + sizeof(SlotNo));
}

uint32_t InternalEntryBytes(const GenericInternalEntry &entry) {
    return static_cast<uint32_t>(sizeof(uint32_t) + entry.key.size() + sizeof(PageNo));
}

uint32_t NodeCapacityBytes(uint16_t special_size) {
    return static_cast<uint32_t>(PAGE_SIZE - sizeof(SlottedPageHeader) - special_size);
}

uint32_t LeafUsedBytes(const std::vector<GenericLeafEntry> &entries) {
    uint32_t used = 0;
    for (const auto &entry : entries) used += LeafEntryBytes(entry) + sizeof(SlotEntry);
    return used;
}

uint32_t InternalUsedBytes(const std::vector<GenericInternalEntry> &entries) {
    uint32_t used = 0;
    for (const auto &entry : entries) used += InternalEntryBytes(entry) + sizeof(SlotEntry);
    return used;
}

bool KeepRetiredPageForRecovery(PageId) {
    // Keep structurally retired pages on disk so restart redo/undo can still fetch
    // them by page id. Reclamation stays logical-only in the transactional path.
    return true;
}

}  // namespace simpledb::generic_btree_helpers
