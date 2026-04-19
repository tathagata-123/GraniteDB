#pragma once

#include <cstdint>

#include "../common/types.h"

namespace simpledb {

struct FrameDesc {
    PageId page_id{0, 0};
    bool is_dirty{false};
    bool is_valid{false};
    uint32_t pin_count{0};
    uint8_t usage_count{0};

    void Reset() {
        page_id = {0, 0};
        is_dirty = false;
        is_valid = false;
        pin_count = 0;
        usage_count = 0;
    }
};

}  // namespace simpledb
