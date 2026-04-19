#include "free_space_map.h"

namespace simpledb {

FreeSpaceMap::FreeSpaceMap(uint16_t bucket_size)
    : bucket_size_(bucket_size == 0 ? 64 : bucket_size),
      num_buckets_(static_cast<uint16_t>(PAGE_SIZE / bucket_size_) + 2),
      buckets_(num_buckets_) {}

void FreeSpaceMap::Clear() {
    std::lock_guard<std::mutex> guard(latch_);
    page_entries_.clear();
    for (auto &bucket : buckets_) bucket.clear();
}

uint16_t FreeSpaceMap::BucketForFreeBytes(uint16_t free_bytes) const {
    uint16_t bucket = static_cast<uint16_t>(free_bytes / bucket_size_);
    if (bucket >= num_buckets_) bucket = static_cast<uint16_t>(num_buckets_ - 1);
    return bucket;
}

void FreeSpaceMap::EnsurePageCapacity(PageNo page_no) {
    if (page_no >= page_entries_.size()) {
        page_entries_.resize(static_cast<std::size_t>(page_no) + 1);
    }
}

void FreeSpaceMap::UpdatePageFreeSpace(PageNo page_no, uint16_t free_bytes) {
    std::lock_guard<std::mutex> guard(latch_);

    EnsurePageCapacity(page_no);
    Entry &entry = page_entries_[page_no];

    if (entry.present) {
        buckets_[entry.bucket].erase(page_no);
    }

    entry.present = true;
    entry.exact_free_bytes = free_bytes;
    entry.bucket = BucketForFreeBytes(free_bytes);
    buckets_[entry.bucket].insert(page_no);
}

void FreeSpaceMap::RemovePage(PageNo page_no) {
    std::lock_guard<std::mutex> guard(latch_);
    if (page_no >= page_entries_.size()) return;
    Entry &entry = page_entries_[page_no];
    if (!entry.present) return;
    buckets_[entry.bucket].erase(page_no);
    entry = Entry{};
}

bool FreeSpaceMap::FindPageWithAtLeast(uint16_t required_free, PageNo *out_page_no) const {
    std::vector<PageNo> candidates = GetCandidatePages(required_free);
    if (candidates.empty()) return false;
    if (out_page_no) *out_page_no = candidates.front();
    return true;
}

std::vector<PageNo> FreeSpaceMap::GetCandidatePages(uint16_t required_free) const {
    std::lock_guard<std::mutex> guard(latch_);
    std::vector<PageNo> result;
    uint16_t start_bucket = BucketForFreeBytes(required_free);

    for (int bucket = static_cast<int>(num_buckets_) - 1; bucket >= static_cast<int>(start_bucket); bucket--) {
        for (PageNo page_no : buckets_[bucket]) {
            const Entry &entry = page_entries_[page_no];
            if (entry.present && entry.exact_free_bytes >= required_free) {
                result.push_back(page_no);
            }
        }
    }
    return result;
}

bool FreeSpaceMap::TryGetFreeSpace(PageNo page_no, uint16_t *out_free_bytes) const {
    std::lock_guard<std::mutex> guard(latch_);
    if (page_no >= page_entries_.size()) return false;
    const Entry &entry = page_entries_[page_no];
    if (!entry.present) return false;
    if (out_free_bytes) *out_free_bytes = entry.exact_free_bytes;
    return true;
}

std::size_t FreeSpaceMap::GetTrackedPageCount() const {
    std::lock_guard<std::mutex> guard(latch_);
    std::size_t count = 0;
    for (const auto &entry : page_entries_) if (entry.present) count++;
    return count;
}

}  // namespace simpledb
