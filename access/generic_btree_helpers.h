#pragma once

#include "generic_btree.h"

namespace simpledb::generic_btree_helpers {

// GenericBTreeIndex has a large amount of page-shape math. Those byte-count and
// occupancy helpers are pure functions, so they live here instead of being
// buried between the recursive structural algorithms.

uint32_t LeafEntryBytes(const GenericLeafEntry &entry);
uint32_t InternalEntryBytes(const GenericInternalEntry &entry);
uint32_t NodeCapacityBytes(uint16_t special_size);
uint32_t LeafUsedBytes(const std::vector<GenericLeafEntry> &entries);
uint32_t InternalUsedBytes(const std::vector<GenericInternalEntry> &entries);
bool KeepRetiredPageForRecovery(PageId page_id);

}  // namespace simpledb::generic_btree_helpers
