#pragma once

#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "wal_records.h"

namespace simpledb {

class LogManager {
public:
    explicit LogManager(const std::string &log_file_path);
    ~LogManager();

    LSN AppendRecord(LogRecord rec);
    void FlushUpTo(LSN lsn);

    LSN GetPersistentLSN() const;
    LSN GetEndOfLogLSN() const;

    std::vector<LogRecord> ReadAllRecordsFrom(LSN start_lsn) const;

    LSN GetMasterCheckpointLSN() const;
    void SetMasterCheckpointLSN(LSN checkpoint_lsn);

private:
    static constexpr std::size_t kMasterRecordSize = sizeof(LSN);

    std::string log_file_path_;
    mutable std::mutex latch_;
    mutable std::fstream file_;
    LSN persistent_lsn_{0};
    LSN next_lsn_{0};
};

}  // namespace simpledb
