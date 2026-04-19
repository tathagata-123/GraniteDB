#pragma once

#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../common/types.h"
#include "../concurrency/page_retire_manager.h"
#include "../recovery/log_manager.h"
#include "../storage/disk_manager.h"
#include "../storage/page.h"
#include "clock_replacer.h"
#include "frame_desc.h"

namespace simpledb {

class BufferPoolManager {
public:
    BufferPoolManager(std::size_t pool_size,
                      DiskManager *disk_manager,
                      LogManager *log_manager = nullptr);
    ~BufferPoolManager();

    Page *FetchPage(PageId page_id);
    Page *NewPage(RelationId relation_id, PageId *out_page_id);
    bool DeletePage(PageId page_id);

    bool UnpinPage(PageId page_id, bool is_dirty);
    bool FlushPage(PageId page_id);
    void FlushAllPages();

    std::size_t GetPoolSize() const { return pool_size_; }
    DiskManager *GetDiskManager() const { return disk_manager_; }
    LogManager *GetLogManager() const { return log_manager_; }
    void SetLogManager(LogManager *log_manager) { log_manager_ = log_manager; }

    PageRetireManager *GetPageRetireManager() { return &page_retire_manager_; }
    const PageRetireManager *GetPageRetireManager() const { return &page_retire_manager_; }

    std::unordered_map<PageId, LSN, PageIdHash> GetDirtyPageTableSnapshot() const;

private:
    bool PrepareFrame(FrameId *out_frame_id);
    void FlushFrame(FrameId frame_id);

    std::size_t pool_size_;
    DiskManager *disk_manager_;
    LogManager *log_manager_;
    std::vector<Page> pages_;
    std::vector<FrameDesc> frame_table_;
    std::unordered_map<PageId, FrameId, PageIdHash> page_table_;
    std::unordered_map<PageId, LSN, PageIdHash> dirty_page_table_;
    ClockReplacer replacer_;
    mutable std::mutex latch_;
    PageRetireManager page_retire_manager_;
};

}  // namespace simpledb
