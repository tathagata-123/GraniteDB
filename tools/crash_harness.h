#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "../access/index.h"
#include "../access/heap_file.h"
#include "../recovery/fault_injector.h"
#include "../recovery/recovery_manager.h"

namespace simpledb {

struct IndexCheckSpec {
    std::string name;
    const AbstractIndex *index{nullptr};
    std::vector<std::size_t> key_column_indexes;
};

class CrashHarness {
public:
    static void ArmCrashPoint(FaultPoint point, uint64_t fire_on_hit = 1);
    static void DisableCrashPoint();

    static void RunMutatingWorkload(const std::function<void()> &workload);

    static bool RecoverAndVerify(RecoveryManager &recovery_manager,
                                 const HeapFile &heap_file,
                                 const std::vector<IndexCheckSpec> &indexes,
                                 std::string *report);
};

}  // namespace simpledb
