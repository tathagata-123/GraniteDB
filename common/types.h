#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace simpledb {

constexpr std::size_t PAGE_SIZE = 8192;

using RelationId = uint32_t;
using PageNo = uint32_t;
using SlotNo = uint16_t;
using LSN = uint64_t;
using TxnId = uint64_t;
using FrameId = int32_t;

enum class TypeId : uint8_t {
    INVALID = 0,
    BOOLEAN,
    INT32,
    INT64,
    DOUBLE,
    VARCHAR
};

struct PageId {
    RelationId relation_id{0};
    PageNo page_no{0};

    bool operator==(const PageId &other) const {
        return relation_id == other.relation_id && page_no == other.page_no;
    }
    bool operator!=(const PageId &other) const {
        return !(*this == other);
    }
};

struct RID {
    PageNo page_no{0};
    SlotNo slot_no{0};

    bool operator==(const RID &other) const {
        return page_no == other.page_no && slot_no == other.slot_no;
    }
};

struct PageIdHash {
    std::size_t operator()(const PageId &pid) const {
        return (static_cast<std::size_t>(pid.relation_id) << 32) ^
               static_cast<std::size_t>(pid.page_no);
    }
};

}  // namespace simpledb
