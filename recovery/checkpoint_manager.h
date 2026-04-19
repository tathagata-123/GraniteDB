#pragma once

#include <unordered_map>

#include "../buffer/buffer_pool_manager.h"
#include "../common/types.h"
#include "../concurrency/transaction.h"
#include "../concurrency/transaction_manager.h"
#include "log_manager.h"

namespace simpledb {

class CheckpointManager {
public:
    CheckpointManager(LogManager *log_manager,
                      TransactionManager *transaction_manager,
                      BufferPoolManager *buffer_pool_manager)
        : log_manager_(log_manager),
          transaction_manager_(transaction_manager),
          buffer_pool_manager_(buffer_pool_manager) {}

    void CreateCheckpoint();

private:
    LogManager *log_manager_;
    TransactionManager *transaction_manager_;
    BufferPoolManager *buffer_pool_manager_;
};

}  // namespace simpledb
