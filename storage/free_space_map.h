#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "../common/types.h"

namespace simpledb {

class FreeSpaceMap {
public:
    explicit FreeSpaceMap(uint16_t bucket_size = 64);

    void Clear();

    void UpdatePageFreeSpace(PageNo page_no, uint16_t free_bytes);
    void RemovePage(PageNo page_no);

    bool FindPageWithAtLeast(uint16_t required_free, PageNo *out_page_no) const;
    std::vector<PageNo> GetCandidatePages(uint16_t required_free) const;

    bool TryGetFreeSpace(PageNo page_no, uint16_t *out_free_bytes) const;
    std::size_t GetTrackedPageCount() const;

private:
    struct Entry {
        bool present{false};
        uint16_t exact_free_bytes{0};
        uint16_t bucket{0};
    };

    uint16_t BucketForFreeBytes(uint16_t free_bytes) const;
    void EnsurePageCapacity(PageNo page_no);

private:
    uint16_t bucket_size_;
    uint16_t num_buckets_;
    mutable std::mutex latch_;
    std::vector<Entry> page_entries_;
    std::vector<std::unordered_set<PageNo>> buckets_;
};

}  // namespace simpledb
