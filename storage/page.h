#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <shared_mutex>

#include "../common/types.h"

namespace simpledb {

class Page {
public:
    Page() { ResetMemory(); }

    void ResetMemory() { std::memset(data_.data(), 0, PAGE_SIZE); }

    char *GetData() { return data_.data(); }
    const char *GetData() const { return data_.data(); }

    void RLatch() { latch_.lock_shared(); }
    bool RTryLatch() { return latch_.try_lock_shared(); }
    void RUnlatch() { latch_.unlock_shared(); }
    void WLatch() { latch_.lock(); }
    bool WTryLatch() { return latch_.try_lock(); }
    void WUnlatch() { latch_.unlock(); }

private:
    alignas(std::max_align_t) std::array<char, PAGE_SIZE> data_{};
    mutable std::shared_mutex latch_;
};

}  // namespace simpledb
