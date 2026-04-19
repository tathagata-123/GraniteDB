#include "crash_harness.h"

#include <cstdlib>
#include <sstream>
#include <stdexcept>

#include "tools/consistency_checker.h"

namespace simpledb {

void CrashHarness::ArmCrashPoint(FaultPoint point, uint64_t fire_on_hit) {
    FaultInjector::Reset();
    std::string point_name = FaultInjector::ToString(point);
    std::string after = std::to_string(fire_on_hit);

    setenv("SIMPLEDB_CRASH_POINT", point_name.c_str(), 1);
    setenv("SIMPLEDB_CRASH_AFTER", after.c_str(), 1);
}

void CrashHarness::DisableCrashPoint() {
    FaultInjector::Reset();
    unsetenv("SIMPLEDB_CRASH_POINT");
    unsetenv("SIMPLEDB_CRASH_AFTER");
}

void CrashHarness::RunMutatingWorkload(const std::function<void()> &workload) {
    if (!workload) {
        throw std::runtime_error("CrashHarness requires a valid workload");
    }
    workload();
}

bool CrashHarness::RecoverAndVerify(RecoveryManager &recovery_manager,
                                    const HeapFile &heap_file,
                                    const std::vector<IndexCheckSpec> &indexes,
                                    std::string *report) {
    std::ostringstream out;

    try {
        recovery_manager.Recover();
        out << "Recovery completed.\n";
    } catch (const std::exception &ex) {
        if (report != nullptr) {
            *report = std::string("Recovery failed: ") + ex.what();
        }
        return false;
    }

    std::string error;
    if (!ConsistencyChecker::VerifyHeapReadable(heap_file, &error)) {
        if (report != nullptr) {
            *report = "Heap verification failed after recovery: " + error;
        }
        return false;
    }
    out << "Heap verification passed.\n";

    for (const auto &spec : indexes) {
        if (spec.index == nullptr) {
            if (report != nullptr) {
                *report = "Null index in crash harness verification";
            }
            return false;
        }

        error.clear();
        if (!ConsistencyChecker::VerifyIndexAgainstHeap(
                heap_file, *spec.index, spec.key_column_indexes, &error)) {
            if (report != nullptr) {
                *report = "Index verification failed for '" + spec.name + "': " + error;
            }
            return false;
        }

        out << "Index verification passed: " << spec.name << "\n";
    }

    if (report != nullptr) {
        *report = out.str();
    }
    return true;
}

}  // namespace simpledb
