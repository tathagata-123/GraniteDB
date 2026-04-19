#pragma once

#include <cstddef>
#include <vector>

#include "frame_desc.h"

namespace simpledb {

class ClockReplacer {
public:
    explicit ClockReplacer(std::size_t pool_size);

    bool Victim(std::vector<FrameDesc> &frame_table, FrameId *out_frame_id);
    void RecordAccess(FrameDesc &frame);

private:
    static constexpr uint8_t kMaxUsageCount = 5;

    std::size_t pool_size_;
    std::size_t clock_hand_;
};

}  // namespace simpledb
