#pragma once

#include "lock_manager.h"
#include "../execution/expressions.h"

#include <optional>
#include <vector>

namespace simpledb::lock_manager_helpers {

// These helpers encode the multiple-granularity and range-locking rules in one
// place. That keeps the public LockManager methods focused on protocol flow:
// validate -> queue -> wait -> grant.

bool IsTableLockMode(LockMode mode);
bool IsTupleOrRangeLockMode(LockMode mode);
LockMode CombineTableModes(LockMode current, LockMode requested);
LockMode CombineTupleOrRangeModes(LockMode current, LockMode requested);
bool TableModeSatisfies(LockMode held, LockMode requested);
bool TupleOrRangeModeSatisfies(LockMode held, LockMode requested);
int CompareKeyVectors(const std::vector<Value> &lhs, const std::vector<Value> &rhs);
bool SameOptionalKey(const std::optional<std::vector<Value>> &lhs,
                     const std::optional<std::vector<Value>> &rhs);
bool RangeEndsBefore(const KeyRangeLockRequest &a, const KeyRangeLockRequest &b);
bool RangesOverlap(const KeyRangeLockRequest &a, const KeyRangeLockRequest &b);

}  // namespace simpledb::lock_manager_helpers
