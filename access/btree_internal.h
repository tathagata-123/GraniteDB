#pragma once

// Small helper routines shared by the split B+ tree implementation files.
//
// The public BTreeIndex interface is unchanged. These helpers are the old local
// utilities that used to sit at the top of one large btree.cpp file. Keeping
// them in a dedicated internal header lets the storage/indexing code be split
// into search, insert, and delete oriented files without changing semantics.

#include "btree.h"

namespace simpledb {
namespace btree_detail {

inline uint32_t LeafEntryBytes(const LeafEntry &entry) {
    return static_cast<uint32_t>(sizeof(uint32_t) + entry.key.size() + sizeof(PageNo) + sizeof(SlotNo));
}

inline uint32_t InternalEntryBytes(const InternalEntry &entry) {
    return static_cast<uint32_t>(sizeof(uint32_t) + entry.key.size() + sizeof(PageNo));
}

inline uint32_t NodeCapacityBytes(uint16_t special_size) {
    return static_cast<uint32_t>(PAGE_SIZE - sizeof(SlottedPageHeader) - special_size);
}

inline uint32_t LeafUsedBytes(const std::vector<LeafEntry> &entries) {
    uint32_t used = 0;
    for (const auto &entry : entries) used += LeafEntryBytes(entry) + sizeof(SlotEntry);
    return used;
}

inline uint32_t InternalUsedBytes(const std::vector<InternalEntry> &entries) {
    uint32_t used = 0;
    for (const auto &entry : entries) used += InternalEntryBytes(entry) + sizeof(SlotEntry);
    return used;
}

inline uint32_t MaxLeafInsertBytes(uint32_t max_key_bytes) {
    return static_cast<uint32_t>(sizeof(uint32_t) + max_key_bytes + sizeof(PageNo) + sizeof(SlotNo) + sizeof(SlotEntry));
}

inline uint32_t MaxInternalInsertBytes(uint32_t max_key_bytes) {
    return static_cast<uint32_t>(sizeof(uint32_t) + max_key_bytes + sizeof(PageNo) + sizeof(SlotEntry));
}

struct WriteLatchFrame {
    PageId pid{};
    Page *page{nullptr};
};

inline void ReleaseWriteFrame(BufferPoolManager *buffer_pool_manager, const WriteLatchFrame &frame, bool is_dirty) {
    if (frame.page == nullptr) return;
    frame.page->WUnlatch();
    buffer_pool_manager->UnpinPage(frame.pid, is_dirty);
}

inline void ReleaseWritePath(BufferPoolManager *buffer_pool_manager, std::vector<WriteLatchFrame> &path) {
    while (!path.empty()) {
        ReleaseWriteFrame(buffer_pool_manager, path.back(), false);
        path.pop_back();
    }
}

inline bool KeepRetiredPageForRecovery(PageId) {
    // Keep structurally retired pages on disk so restart redo/undo can still fetch
    // them by page id. Reclamation stays logical-only in the transactional path.
    return true;
}

}  // namespace btree_detail
}  // namespace simpledb
