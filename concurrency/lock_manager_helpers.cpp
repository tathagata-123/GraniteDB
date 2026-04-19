#include "lock_manager_helpers.h"

#include <stdexcept>

namespace simpledb::lock_manager_helpers {

bool IsTableLockMode(LockMode mode) {
    return mode == LockMode::IS || mode == LockMode::IX || mode == LockMode::S ||
           mode == LockMode::SIX || mode == LockMode::X;
}

bool IsTupleOrRangeLockMode(LockMode mode) {
    return mode == LockMode::S || mode == LockMode::X;
}

LockMode CombineTableModes(LockMode current, LockMode requested) {
    if (current == requested) return current;
    if (current == LockMode::X || requested == LockMode::X) return LockMode::X;
    if (current == LockMode::SIX || requested == LockMode::SIX) return LockMode::SIX;
    if ((current == LockMode::S && requested == LockMode::IX) ||
        (current == LockMode::IX && requested == LockMode::S)) {
        return LockMode::SIX;
    }
    if (current == LockMode::S || requested == LockMode::S) return LockMode::S;
    if (current == LockMode::IX || requested == LockMode::IX) return LockMode::IX;
    return LockMode::IS;
}

LockMode CombineTupleOrRangeModes(LockMode current, LockMode requested) {
    if (current == LockMode::X || requested == LockMode::X) return LockMode::X;
    return LockMode::S;
}

bool TableModeSatisfies(LockMode held, LockMode requested) {
    return CombineTableModes(held, requested) == held;
}

bool TupleOrRangeModeSatisfies(LockMode held, LockMode requested) {
    return CombineTupleOrRangeModes(held, requested) == held;
}

int CompareKeyVectors(const std::vector<Value> &lhs, const std::vector<Value> &rhs) {
    if (lhs.size() != rhs.size()) {
        throw std::runtime_error("Key-range lock comparison requires equal-length key vectors");
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        int cmp = CompareValues(lhs[i], rhs[i]);
        if (cmp != 0) return cmp;
    }
    return 0;
}

bool SameOptionalKey(const std::optional<std::vector<Value>> &lhs,
                     const std::optional<std::vector<Value>> &rhs) {
    if (lhs.has_value() != rhs.has_value()) return false;
    if (!lhs.has_value()) return true;
    return CompareKeyVectors(*lhs, *rhs) == 0;
}

bool RangeEndsBefore(const KeyRangeLockRequest &a, const KeyRangeLockRequest &b) {
    if (a.upper_unbounded || b.lower_unbounded) return false;
    int cmp = CompareKeyVectors(a.upper_key, b.lower_key);
    if (cmp < 0) return true;
    if (cmp > 0) return false;
    return !(a.upper_inclusive && b.lower_inclusive);
}

bool RangesOverlap(const KeyRangeLockRequest &a, const KeyRangeLockRequest &b) {
    return !RangeEndsBefore(a, b) && !RangeEndsBefore(b, a);
}

}  // namespace simpledb::lock_manager_helpers
