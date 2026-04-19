#include "page_retire_manager.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace simpledb {

PageRetireManager::OperationGuard::OperationGuard(PageRetireManager *manager)
    : manager_(manager), epoch_(manager_ != nullptr ? manager_->EnterEpoch() : 0) {}

PageRetireManager::OperationGuard::OperationGuard(OperationGuard &&other) noexcept
    : manager_(other.manager_), epoch_(other.epoch_) {
    other.manager_ = nullptr;
    other.epoch_ = 0;
}

PageRetireManager::OperationGuard &PageRetireManager::OperationGuard::operator=(OperationGuard &&other) noexcept {
    if (this == &other) return *this;
    if (manager_ != nullptr) manager_->ExitEpoch(epoch_);
    manager_ = other.manager_;
    epoch_ = other.epoch_;
    other.manager_ = nullptr;
    other.epoch_ = 0;
    return *this;
}

PageRetireManager::OperationGuard::~OperationGuard() {
    if (manager_ != nullptr) manager_->ExitEpoch(epoch_);
}

PageRetireManager::PageRetireManager() : global_epoch_(1) {}

PageRetireManager::OperationGuard PageRetireManager::Guard() { return OperationGuard(this); }

uint64_t PageRetireManager::EnterEpoch() {
    uint64_t epoch = global_epoch_.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> guard(latch_);
    active_epochs_[epoch]++;
    return epoch;
}

void PageRetireManager::ExitEpoch(uint64_t epoch) {
    std::lock_guard<std::mutex> guard(latch_);
    auto it = active_epochs_.find(epoch);
    if (it != active_epochs_.end()) {
        if (it->second <= 1) active_epochs_.erase(it);
        else it->second--;
    }
    TryReclaimLocked();
}

void PageRetireManager::RetirePage(PageId page_id, ReclaimCallback callback) {
    std::lock_guard<std::mutex> guard(latch_);
    uint64_t retire_epoch = global_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    retired_pages_.push_back(RetiredPageEntry{page_id, retire_epoch, std::move(callback)});
    TryReclaimLocked();
}

void PageRetireManager::Drain() {
    std::lock_guard<std::mutex> guard(latch_);
    global_epoch_.fetch_add(1, std::memory_order_acq_rel);
    TryReclaimLocked();
}

uint64_t PageRetireManager::ComputeSafeEpochLocked() const {
    if (active_epochs_.empty()) {
        return global_epoch_.load(std::memory_order_acquire);
    }
    uint64_t min_epoch = std::numeric_limits<uint64_t>::max();
    for (const auto &entry : active_epochs_) {
        min_epoch = std::min(min_epoch, entry.first);
    }
    return (min_epoch == 0 ? 0 : min_epoch - 1);
}

void PageRetireManager::TryReclaimLocked() {
    uint64_t safe_epoch = ComputeSafeEpochLocked();
    std::vector<RetiredPageEntry> pending;
    pending.reserve(retired_pages_.size());
    for (auto &entry : retired_pages_) {
        if (entry.retire_epoch > safe_epoch) {
            pending.push_back(std::move(entry));
            continue;
        }
        if (!entry.reclaim(entry.page_id)) {
            pending.push_back(std::move(entry));
        }
    }
    retired_pages_.swap(pending);
}

}  // namespace simpledb
