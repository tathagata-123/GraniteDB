#pragma once

#include "../buffer/buffer_pool_manager.h"

namespace simpledb {

struct ExecContext {
    BufferPoolManager *buffer_pool_manager{nullptr};
};

}  // namespace simpledb
