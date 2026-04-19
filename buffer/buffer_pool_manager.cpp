#include "buffer_pool_manager.h"

#include <algorithm>
#include <stdexcept>

#include "../storage/page_lsn_util.h"
#include "../recovery/fault_injector.h"

namespace simpledb {

BufferPoolManager::BufferPoolManager(std::size_t pool_size,
                                     DiskManager *disk_manager,
                                     LogManager *log_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager), log_manager_(log_manager),
      pages_(pool_size), frame_table_(pool_size), replacer_(pool_size) {
    for (auto &frame : frame_table_) frame.Reset();
}

BufferPoolManager::~BufferPoolManager() {
    page_retire_manager_.Drain();
    FlushAllPages();
}

void BufferPoolManager::FlushFrame(FrameId frame_id) {
    FrameDesc &frame = frame_table_[frame_id];
    if (frame.is_valid && frame.is_dirty) {
        LSN page_lsn = GetPageLSN(&pages_[frame_id]);
        if (log_manager_ != nullptr) log_manager_->FlushUpTo(page_lsn);
        FaultInjector::MaybeCrash(FaultPoint::BEFORE_PAGE_FLUSH);
        disk_manager_->WritePage(frame.page_id, pages_[frame_id].GetData());
        disk_manager_->Sync(frame.page_id.relation_id);
        FaultInjector::MaybeCrash(FaultPoint::AFTER_PAGE_FLUSH);
        frame.is_dirty = false;
        dirty_page_table_.erase(frame.page_id);
    }
}

bool BufferPoolManager::PrepareFrame(FrameId *out_frame_id) {
    FrameId victim = -1;
    if (!replacer_.Victim(frame_table_, &victim)) return false;
    FrameDesc &frame = frame_table_[victim];
    if (frame.is_valid) {
        FlushFrame(victim);
        page_table_.erase(frame.page_id);
    }
    pages_[victim].ResetMemory();
    frame.Reset();
    *out_frame_id = victim;
    return true;
}

Page *BufferPoolManager::FetchPage(PageId page_id) {
    std::lock_guard<std::mutex> guard(latch_);
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        FrameId frame_id = it->second;
        FrameDesc &frame = frame_table_[frame_id];
        frame.pin_count++;
        replacer_.RecordAccess(frame);
        return &pages_[frame_id];
    }
    if (page_id.page_no >= disk_manager_->GetNumPages(page_id.relation_id)) return nullptr;
    FrameId frame_id = -1;
    if (!PrepareFrame(&frame_id)) return nullptr;
    disk_manager_->ReadPage(page_id, pages_[frame_id].GetData());
    FrameDesc &frame = frame_table_[frame_id];
    frame.page_id = page_id; frame.is_valid = true; frame.is_dirty = false; frame.pin_count = 1; frame.usage_count = 0;
    replacer_.RecordAccess(frame);
    page_table_[page_id] = frame_id;
    return &pages_[frame_id];
}

Page *BufferPoolManager::NewPage(RelationId relation_id, PageId *out_page_id) {
    std::lock_guard<std::mutex> guard(latch_);
    FrameId frame_id = -1;
    if (!PrepareFrame(&frame_id)) return nullptr;
    PageId new_page_id = disk_manager_->AllocatePage(relation_id);
    pages_[frame_id].ResetMemory();
    SetPageLSN(&pages_[frame_id], 0);
    FrameDesc &frame = frame_table_[frame_id];
    frame.page_id = new_page_id; frame.is_valid = true; frame.is_dirty = false; frame.pin_count = 1; frame.usage_count = 0;
    replacer_.RecordAccess(frame);
    page_table_[new_page_id] = frame_id;
    if (out_page_id != nullptr) *out_page_id = new_page_id;
    return &pages_[frame_id];
}

bool BufferPoolManager::DeletePage(PageId page_id) {
    std::lock_guard<std::mutex> guard(latch_);
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        FrameId frame_id = it->second;
        FrameDesc &frame = frame_table_[frame_id];
        if (frame.pin_count > 0) return false;
        dirty_page_table_.erase(page_id);
        page_table_.erase(it);
        pages_[frame_id].ResetMemory();
        frame.Reset();
    }
    disk_manager_->DeallocatePage(page_id);
    return true;
}

bool BufferPoolManager::UnpinPage(PageId page_id, bool is_dirty) {
    std::lock_guard<std::mutex> guard(latch_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return false;
    FrameId frame_id = it->second;
    FrameDesc &frame = frame_table_[frame_id];
    if (frame.pin_count == 0) return false;
    frame.pin_count--;
    if (is_dirty) {
        LSN rec_lsn = GetPageLSN(&pages_[frame_id]);
        auto dpt_it = dirty_page_table_.find(page_id);
        if (dpt_it == dirty_page_table_.end()) {
            dirty_page_table_.emplace(page_id, rec_lsn);
        } else {
            dpt_it->second = std::min(dpt_it->second, rec_lsn);
        }
        frame.is_dirty = true;
    }
    return true;
}

bool BufferPoolManager::FlushPage(PageId page_id) {
    std::lock_guard<std::mutex> guard(latch_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return false;
    FlushFrame(it->second);
    return true;
}

void BufferPoolManager::FlushAllPages() {
    std::lock_guard<std::mutex> guard(latch_);
    for (FrameId frame_id = 0; frame_id < static_cast<FrameId>(pool_size_); frame_id++) {
        if (frame_table_[frame_id].is_valid) FlushFrame(frame_id);
    }
}

std::unordered_map<PageId, LSN, PageIdHash> BufferPoolManager::GetDirtyPageTableSnapshot() const {
    std::lock_guard<std::mutex> guard(latch_);
    return dirty_page_table_;
}

}  // namespace simpledb
