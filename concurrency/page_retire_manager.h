#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../common/types.h"

namespace simpledb {

class PageRetireManager {
public:
    using ReclaimCallback = std::function<bool(PageId)>;

    class OperationGuard {
    public:
        explicit OperationGuard(PageRetireManager *manager = nullptr);
        OperationGuard(const OperationGuard &) = delete;
        OperationGuard &operator=(const OperationGuard &) = delete;
        OperationGuard(OperationGuard &&other) noexcept;
        OperationGuard &operator=(OperationGuard &&other) noexcept;
        ~OperationGuard();

    private:
        PageRetireManager *manager_;
        uint64_t epoch_;
    };

    PageRetireManager();

    OperationGuard Guard();
    uint64_t EnterEpoch();
    void ExitEpoch(uint64_t epoch);

    void RetirePage(PageId page_id, ReclaimCallback callback);
    void Drain();

private:
    struct RetiredPageEntry {
        PageId page_id{};
        uint64_t retire_epoch{0};
        ReclaimCallback reclaim;
    };

    void TryReclaimLocked();
    uint64_t ComputeSafeEpochLocked() const;

    std::atomic<uint64_t> global_epoch_;
    mutable std::mutex latch_;
    std::unordered_map<uint64_t, uint64_t> active_epochs_;
    std::vector<RetiredPageEntry> retired_pages_;
};

}  // namespace simpledb
