#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "../common/types.h"
#include "../concurrency/transaction.h"

namespace simpledb {

enum class LogRecordType : uint8_t {
    INVALID = 0,
    BEGIN,
    COMMIT,
    ABORT,
    HEAP_INSERT,
    HEAP_DELETE,
    HEAP_UPDATE,
    BTREE_INSERT,
    BTREE_DELETE,
    BTREE_PAGE_SPLIT,
    BTREE_REBALANCE,
    BTREE_MERGE,
    BTREE_META_UPDATE,
    BEGIN_CHECKPOINT,
    END_CHECKPOINT,
    CLR
};

struct CheckpointTxnEntry {
    TxnId txn_id{0};
    TransactionState state{TransactionState::ACTIVE};
    LSN last_lsn{0};
};

struct DirtyPageEntry {
    PageId page_id{};
    LSN rec_lsn{0};
};

struct LogRecord {
    LogRecordType type{LogRecordType::INVALID};
    LSN lsn{0};
    TxnId txn_id{0};
    LSN prev_lsn{0};
    LSN undo_next_lsn{0};
    bool has_page{false};
    PageId page_id{};
    std::vector<char> before_image;
    std::vector<char> after_image;
    std::vector<CheckpointTxnEntry> active_txns;
    std::vector<DirtyPageEntry> dirty_pages;
};

inline void WalWriteBytes(std::vector<char> &out, const void *src, std::size_t len) {
    const char *p = static_cast<const char *>(src);
    out.insert(out.end(), p, p + len);
}

template <typename T>
inline void WalWritePod(std::vector<char> &out, const T &value) {
    WalWriteBytes(out, &value, sizeof(T));
}

inline void WalWriteVector(std::vector<char> &out, const std::vector<char> &bytes) {
    uint32_t sz = static_cast<uint32_t>(bytes.size());
    WalWritePod(out, sz);
    if (!bytes.empty()) {
        WalWriteBytes(out, bytes.data(), bytes.size());
    }
}

inline std::vector<char> SerializeLogRecordPayload(const LogRecord &rec) {
    std::vector<char> out;
    WalWritePod(out, rec.type);
    WalWritePod(out, rec.lsn);
    WalWritePod(out, rec.txn_id);
    WalWritePod(out, rec.prev_lsn);
    WalWritePod(out, rec.undo_next_lsn);
    WalWritePod(out, rec.has_page);
    if (rec.has_page) {
        WalWritePod(out, rec.page_id.relation_id);
        WalWritePod(out, rec.page_id.page_no);
    }
    WalWriteVector(out, rec.before_image);
    WalWriteVector(out, rec.after_image);

    uint32_t txn_cnt = static_cast<uint32_t>(rec.active_txns.size());
    WalWritePod(out, txn_cnt);
    for (const auto &entry : rec.active_txns) {
        WalWritePod(out, entry.txn_id);
        uint8_t state = static_cast<uint8_t>(entry.state);
        WalWritePod(out, state);
        WalWritePod(out, entry.last_lsn);
    }

    uint32_t dpt_cnt = static_cast<uint32_t>(rec.dirty_pages.size());
    WalWritePod(out, dpt_cnt);
    for (const auto &entry : rec.dirty_pages) {
        WalWritePod(out, entry.page_id.relation_id);
        WalWritePod(out, entry.page_id.page_no);
        WalWritePod(out, entry.rec_lsn);
    }
    return out;
}

class WalPayloadReader {
public:
    WalPayloadReader(const char *data, std::size_t len) : data_(data), len_(len), pos_(0) {}

    template <typename T>
    T ReadPod() {
        if (pos_ + sizeof(T) > len_) {
            throw std::runtime_error("WAL payload underflow");
        }
        T v{};
        std::memcpy(&v, data_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }

    std::vector<char> ReadVector() {
        uint32_t sz = ReadPod<uint32_t>();
        if (pos_ + sz > len_) {
            throw std::runtime_error("WAL vector length out of bounds");
        }
        std::vector<char> out(sz);
        if (sz > 0) {
            std::memcpy(out.data(), data_ + pos_, sz);
        }
        pos_ += sz;
        return out;
    }

private:
    const char *data_;
    std::size_t len_;
    std::size_t pos_;
};

inline LogRecord DeserializeLogRecordPayload(const char *data, std::size_t len) {
    WalPayloadReader rd(data, len);
    LogRecord rec;
    rec.type = rd.ReadPod<LogRecordType>();
    rec.lsn = rd.ReadPod<LSN>();
    rec.txn_id = rd.ReadPod<TxnId>();
    rec.prev_lsn = rd.ReadPod<LSN>();
    rec.undo_next_lsn = rd.ReadPod<LSN>();
    rec.has_page = rd.ReadPod<bool>();
    if (rec.has_page) {
        rec.page_id.relation_id = rd.ReadPod<RelationId>();
        rec.page_id.page_no = rd.ReadPod<PageNo>();
    }
    rec.before_image = rd.ReadVector();
    rec.after_image = rd.ReadVector();

    uint32_t txn_cnt = rd.ReadPod<uint32_t>();
    for (uint32_t i = 0; i < txn_cnt; i++) {
        CheckpointTxnEntry entry;
        entry.txn_id = rd.ReadPod<TxnId>();
        entry.state = static_cast<TransactionState>(rd.ReadPod<uint8_t>());
        entry.last_lsn = rd.ReadPod<LSN>();
        rec.active_txns.push_back(entry);
    }

    uint32_t dpt_cnt = rd.ReadPod<uint32_t>();
    for (uint32_t i = 0; i < dpt_cnt; i++) {
        DirtyPageEntry entry;
        entry.page_id.relation_id = rd.ReadPod<RelationId>();
        entry.page_id.page_no = rd.ReadPod<PageNo>();
        entry.rec_lsn = rd.ReadPod<LSN>();
        rec.dirty_pages.push_back(entry);
    }
    return rec;
}

inline LogRecord MakePageImageLogRecord(LogRecordType type,
                                        TxnId txn_id,
                                        LSN prev_lsn,
                                        PageId page_id,
                                        const std::vector<char> &before_image,
                                        const std::vector<char> &after_image) {
    LogRecord rec;
    rec.type = type;
    rec.txn_id = txn_id;
    rec.prev_lsn = prev_lsn;
    rec.has_page = true;
    rec.page_id = page_id;
    rec.before_image = before_image;
    rec.after_image = after_image;
    return rec;
}

inline LogRecord MakeClrPageUndoRecord(TxnId txn_id,
                                       LSN prev_lsn,
                                       LSN undo_next_lsn,
                                       PageId page_id,
                                       const std::vector<char> &redo_image) {
    LogRecord clr;
    clr.type = LogRecordType::CLR;
    clr.txn_id = txn_id;
    clr.prev_lsn = prev_lsn;
    clr.undo_next_lsn = undo_next_lsn;
    clr.has_page = true;
    clr.page_id = page_id;
    clr.after_image = redo_image;
    return clr;
}

inline bool IsIndexStructuralRecord(LogRecordType t) {
    return t == LogRecordType::BTREE_PAGE_SPLIT ||
           t == LogRecordType::BTREE_REBALANCE ||
           t == LogRecordType::BTREE_MERGE ||
           t == LogRecordType::BTREE_META_UPDATE;
}

inline bool IsUndoablePageRecord(LogRecordType t) {
    return t == LogRecordType::HEAP_INSERT ||
           t == LogRecordType::HEAP_DELETE ||
           t == LogRecordType::HEAP_UPDATE ||
           t == LogRecordType::BTREE_INSERT ||
           t == LogRecordType::BTREE_DELETE ||
           t == LogRecordType::BTREE_PAGE_SPLIT ||
           t == LogRecordType::BTREE_REBALANCE ||
           t == LogRecordType::BTREE_MERGE ||
           t == LogRecordType::BTREE_META_UPDATE;
}

inline bool IsPageUpdateRecord(LogRecordType t) {
    return IsUndoablePageRecord(t) || t == LogRecordType::CLR;
}

}  // namespace simpledb
