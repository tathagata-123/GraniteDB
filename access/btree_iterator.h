#pragma once

#include <utility>
#include <vector>

#include "../common/value.h"
#include "btree.h"

namespace simpledb {

class BTreeIndexIterator {
public:
    explicit BTreeIndexIterator(const BTreeIndex *tree);
    BTreeIndexIterator(const BTreeIndex *tree, const Value &lower_bound);

    bool HasNext() const;
    std::pair<Value, RID> Next();

private:
    void AdvanceToNextValid();

    const BTreeIndex *tree_;
    PageNo current_leaf_page_no_;
    std::size_t current_entry_index_;
    bool at_end_;
    bool has_lower_bound_;
    std::vector<char> lower_bound_key_;
};

}  // namespace simpledb
