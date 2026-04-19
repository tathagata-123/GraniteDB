#include "log_manager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

#include "fault_injector.h"
#include "../common/durable_io.h"

namespace simpledb {

namespace {

struct WalScanResult {
    LSN valid_end{0};
    std::unordered_map<LSN, LogRecordType> record_types;
    bool repaired_tail{false};
    bool rewrote_master{false};
};

WalScanResult ScanWalFile(const std::string &path, std::uintmax_t file_size) {
    WalScanResult result;
    result.valid_end = static_cast<LSN>(file_size);

    if (file_size <= sizeof(LSN)) {
        result.valid_end = static_cast<LSN>(sizeof(LSN));
        return result;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("Failed to open WAL file for validation");
    }

    LSN master = 0;
    in.read(reinterpret_cast<char *>(&master), sizeof(master));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(master))) {
        result.valid_end = static_cast<LSN>(sizeof(LSN));
        result.repaired_tail = true;
        return result;
    }

    LSN offset = static_cast<LSN>(sizeof(LSN));
    while (offset < static_cast<LSN>(file_size)) {
        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        uint32_t total_size = 0;
        in.read(reinterpret_cast<char *>(&total_size), sizeof(total_size));
        if (in.gcount() != static_cast<std::streamsize>(sizeof(total_size))) {
            result.valid_end = offset;
            result.repaired_tail = true;
            break;
        }

        if (total_size < sizeof(uint32_t)) {
            result.valid_end = offset;
            result.repaired_tail = true;
            break;
        }

        const LSN next_offset = offset + total_size;
        if (next_offset > static_cast<LSN>(file_size)) {
            result.valid_end = offset;
            result.repaired_tail = true;
            break;
        }

        const uint32_t payload_size = total_size - sizeof(uint32_t);
        std::vector<char> payload(payload_size);
        if (payload_size > 0) {
            in.read(payload.data(), static_cast<std::streamsize>(payload_size));
            if (in.gcount() != static_cast<std::streamsize>(payload_size)) {
                result.valid_end = offset;
                result.repaired_tail = true;
                break;
            }
        }

        try {
            LogRecord rec = DeserializeLogRecordPayload(payload.data(), payload.size());
            result.record_types[offset] = rec.type;
        } catch (const std::exception &) {
            result.valid_end = offset;
            result.repaired_tail = true;
            break;
        }

        offset = next_offset;
    }

    return result;
}

void EnsureWalInitializedAndTrimmed(const std::string &path) {
    namespace fs = std::filesystem;

    if (!fs::exists(path) || fs::file_size(path) < sizeof(LSN)) {
        std::ofstream create(path, std::ios::binary | std::ios::trunc);
        if (!create.is_open()) {
            throw std::runtime_error("Failed to create WAL file");
        }

        LSN master = 0;
        create.write(reinterpret_cast<const char *>(&master), sizeof(master));
        create.flush();
        create.close();
        DurableSyncPath(path);
        DurableSyncParentDirectory(path);
        return;
    }

    const std::uintmax_t file_size = fs::file_size(path);
    WalScanResult scan = ScanWalFile(path, file_size);

    if (scan.valid_end < static_cast<LSN>(file_size)) {
        fs::resize_file(path, static_cast<std::uintmax_t>(scan.valid_end));
        DurableSyncPath(path);
        DurableSyncParentDirectory(path);
        scan.repaired_tail = true;
    }

    std::fstream wal(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!wal.is_open()) {
        throw std::runtime_error("Failed to open WAL file for repair");
    }

    LSN master = 0;
    wal.seekg(0, std::ios::beg);
    wal.read(reinterpret_cast<char *>(&master), sizeof(master));
    if (wal.gcount() != static_cast<std::streamsize>(sizeof(master))) {
        throw std::runtime_error("Failed to read WAL master record during repair");
    }

    bool master_invalid = false;
    if (master != 0) {
        auto it = scan.record_types.find(master);
        if (master < static_cast<LSN>(sizeof(LSN)) || master >= scan.valid_end ||
            it == scan.record_types.end() || it->second != LogRecordType::BEGIN_CHECKPOINT) {
            master_invalid = true;
        }
    }

    if (master_invalid) {
        const LSN zero = 0;
        wal.clear();
        wal.seekp(0, std::ios::beg);
        wal.write(reinterpret_cast<const char *>(&zero), sizeof(zero));
        wal.flush();
        if (!wal.good()) {
            throw std::runtime_error("Failed to repair WAL master checkpoint record");
        }
        DurableSyncPath(path);
        scan.rewrote_master = true;
    }
}

}  // namespace

LogManager::LogManager(const std::string &log_file_path) : log_file_path_(log_file_path) {
    EnsureWalInitializedAndTrimmed(log_file_path_);

    file_.open(log_file_path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!file_.is_open()) {
        throw std::runtime_error("Failed to open WAL file");
    }

    file_.seekg(0, std::ios::end);
    next_lsn_ = static_cast<LSN>(file_.tellg());
    persistent_lsn_ = next_lsn_;
}

LogManager::~LogManager() {
    std::lock_guard<std::mutex> guard(latch_);
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

LSN LogManager::AppendRecord(LogRecord rec) {
    std::lock_guard<std::mutex> guard(latch_);

    FaultInjector::MaybeCrash(FaultPoint::BEFORE_WAL_APPEND);
    file_.seekp(0, std::ios::end);
    LSN lsn = static_cast<LSN>(file_.tellp());
    rec.lsn = lsn;

    std::vector<char> payload = SerializeLogRecordPayload(rec);
    uint32_t total_size = static_cast<uint32_t>(sizeof(uint32_t) + payload.size());

    file_.write(reinterpret_cast<const char *>(&total_size), sizeof(total_size));
    if (!payload.empty()) {
        file_.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    if (!file_.good()) {
        throw std::runtime_error("Failed to append WAL record");
    }

    next_lsn_ = lsn + total_size;
    FaultInjector::MaybeCrash(FaultPoint::AFTER_WAL_APPEND);
    return lsn;
}

void LogManager::FlushUpTo(LSN lsn) {
    std::lock_guard<std::mutex> guard(latch_);

    if (lsn <= persistent_lsn_) {
        return;
    }

    FaultInjector::MaybeCrash(FaultPoint::BEFORE_WAL_FLUSH);
    file_.flush();
    if (!file_.good()) {
        throw std::runtime_error("Failed to flush WAL");
    }
    DurableSyncPath(log_file_path_);

    FaultInjector::MaybeCrash(FaultPoint::AFTER_WAL_FLUSH);
    persistent_lsn_ = std::min(lsn, next_lsn_);
}

LSN LogManager::GetPersistentLSN() const {
    std::lock_guard<std::mutex> guard(latch_);
    return persistent_lsn_;
}

LSN LogManager::GetEndOfLogLSN() const {
    std::lock_guard<std::mutex> guard(latch_);
    return next_lsn_;
}

std::vector<LogRecord> LogManager::ReadAllRecordsFrom(LSN start_lsn) const {
    std::lock_guard<std::mutex> guard(latch_);

    std::vector<LogRecord> records;
    std::ifstream in(log_file_path_, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("Failed to open WAL file for reading");
    }

    if (start_lsn < kMasterRecordSize) {
        start_lsn = kMasterRecordSize;
    }

    in.seekg(0, std::ios::end);
    LSN file_end = static_cast<LSN>(in.tellg());
    if (start_lsn >= file_end) {
        return records;
    }

    in.seekg(static_cast<std::streamoff>(start_lsn), std::ios::beg);

    while (true) {
        std::streamoff rec_pos = in.tellg();
        if (rec_pos < 0 || static_cast<LSN>(rec_pos) >= file_end) {
            break;
        }

        uint32_t total_size = 0;
        in.read(reinterpret_cast<char *>(&total_size), sizeof(total_size));
        if (in.gcount() == 0) {
            break;
        }
        if (in.gcount() != static_cast<std::streamsize>(sizeof(total_size))) {
            break;
        }

        if (total_size < sizeof(uint32_t)) {
            throw std::runtime_error("Corrupt WAL record size");
        }

        uint32_t payload_size = total_size - sizeof(uint32_t);
        if (static_cast<LSN>(rec_pos) + total_size > file_end) {
            break;
        }

        std::vector<char> payload(payload_size);
        if (payload_size > 0) {
            in.read(payload.data(), static_cast<std::streamsize>(payload_size));
            if (in.gcount() != static_cast<std::streamsize>(payload_size)) {
                break;
            }
        }

        LogRecord rec = DeserializeLogRecordPayload(payload.data(), payload.size());
        rec.lsn = static_cast<LSN>(rec_pos);
        records.push_back(std::move(rec));
    }

    return records;
}

LSN LogManager::GetMasterCheckpointLSN() const {
    std::lock_guard<std::mutex> guard(latch_);

    std::ifstream in(log_file_path_, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("Failed to open WAL file for master record");
    }

    LSN master = 0;
    in.read(reinterpret_cast<char *>(&master), sizeof(master));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(master))) {
        throw std::runtime_error("Failed to read WAL master checkpoint record");
    }
    return master;
}

void LogManager::SetMasterCheckpointLSN(LSN checkpoint_lsn) {
    std::lock_guard<std::mutex> guard(latch_);

    file_.seekp(0, std::ios::beg);
    file_.write(reinterpret_cast<const char *>(&checkpoint_lsn), sizeof(checkpoint_lsn));
    if (!file_.good()) {
        throw std::runtime_error("Failed to update master checkpoint LSN");
    }
    file_.flush();
    if (!file_.good()) {
        throw std::runtime_error("Failed to update master checkpoint LSN");
    }
    DurableSyncPath(log_file_path_);
}

}  // namespace simpledb
