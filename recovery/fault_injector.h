#pragma once

#include <cstdint>

namespace simpledb {

enum class FaultPoint {
    BEFORE_WAL_APPEND,
    AFTER_WAL_APPEND,
    BEFORE_WAL_FLUSH,
    AFTER_WAL_FLUSH,
    BEFORE_PAGE_FLUSH,
    AFTER_PAGE_FLUSH,
    BEFORE_REDO_APPLY,
    AFTER_REDO_APPLY,
    BEFORE_UNDO_APPLY,
    AFTER_UNDO_APPLY
};

class FaultInjector {
public:
    static const char *ToString(FaultPoint point);
    static void MaybeCrash(FaultPoint point);
    static void Reset();
};

}  // namespace simpledb
