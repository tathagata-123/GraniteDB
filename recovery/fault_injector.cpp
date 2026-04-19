#include "fault_injector.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace simpledb {

namespace {

std::mutex g_fault_latch;
std::unordered_map<std::string, uint64_t> g_hit_count;

uint64_t ReadCrashAfter() {
    const char *env = std::getenv("SIMPLEDB_CRASH_AFTER");
    if (env == nullptr || *env == '\0') {
        return 1;
    }

    char *end = nullptr;
    unsigned long long value = std::strtoull(env, &end, 10);
    if (end == env || value == 0) {
        return 1;
    }
    return static_cast<uint64_t>(value);
}

}  // namespace

const char *FaultInjector::ToString(FaultPoint point) {
    switch (point) {
        case FaultPoint::BEFORE_WAL_APPEND: return "BEFORE_WAL_APPEND";
        case FaultPoint::AFTER_WAL_APPEND: return "AFTER_WAL_APPEND";
        case FaultPoint::BEFORE_WAL_FLUSH: return "BEFORE_WAL_FLUSH";
        case FaultPoint::AFTER_WAL_FLUSH: return "AFTER_WAL_FLUSH";
        case FaultPoint::BEFORE_PAGE_FLUSH: return "BEFORE_PAGE_FLUSH";
        case FaultPoint::AFTER_PAGE_FLUSH: return "AFTER_PAGE_FLUSH";
        case FaultPoint::BEFORE_REDO_APPLY: return "BEFORE_REDO_APPLY";
        case FaultPoint::AFTER_REDO_APPLY: return "AFTER_REDO_APPLY";
        case FaultPoint::BEFORE_UNDO_APPLY: return "BEFORE_UNDO_APPLY";
        case FaultPoint::AFTER_UNDO_APPLY: return "AFTER_UNDO_APPLY";
    }
    return "UNKNOWN";
}

void FaultInjector::MaybeCrash(FaultPoint point) {
    const char *wanted = std::getenv("SIMPLEDB_CRASH_POINT");
    if (wanted == nullptr || *wanted == '\0') {
        return;
    }

    const char *current = ToString(point);
    if (std::strcmp(wanted, current) != 0) {
        return;
    }

    std::lock_guard<std::mutex> guard(g_fault_latch);
    uint64_t &hits = g_hit_count[current];
    hits++;

    uint64_t crash_after = ReadCrashAfter();
    if (hits < crash_after) {
        return;
    }

    std::fflush(nullptr);
    std::_Exit(88);
}

void FaultInjector::Reset() {
    std::lock_guard<std::mutex> guard(g_fault_latch);
    g_hit_count.clear();
}

}  // namespace simpledb
