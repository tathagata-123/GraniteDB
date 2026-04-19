#include "clock_replacer.h"

namespace simpledb {

ClockReplacer::ClockReplacer(std::size_t pool_size)
    : pool_size_(pool_size), clock_hand_(0) {}

void ClockReplacer::RecordAccess(FrameDesc &frame) {
    if (frame.usage_count < kMaxUsageCount) {
        frame.usage_count++;
    }
}

bool ClockReplacer::Victim(std::vector<FrameDesc> &frame_table, FrameId *out_frame_id) {
    if (pool_size_ == 0) return false;

    const std::size_t max_steps = pool_size_ * (kMaxUsageCount + 2);

    for (std::size_t step = 0; step < max_steps; step++) {
        FrameId current = static_cast<FrameId>(clock_hand_);
        FrameDesc &frame = frame_table[current];
        clock_hand_ = (clock_hand_ + 1) % pool_size_;

        if (!frame.is_valid) {
            *out_frame_id = current;
            return true;
        }
        if (frame.pin_count > 0) {
            continue;
        }
        if (frame.usage_count == 0) {
            *out_frame_id = current;
            return true;
        }
        frame.usage_count--;
    }

    return false;
}

}  // namespace simpledb
