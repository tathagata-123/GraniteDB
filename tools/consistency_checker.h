#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "../access/btree.h"
#include "../access/index.h"
#include "../access/generic_btree.h"
#include "../access/heap_file.h"

namespace simpledb {

class ConsistencyChecker {
public:
    static bool VerifyHeapReadable(const HeapFile &heap_file, std::string *error);

    static bool VerifyIndexAgainstHeap(const HeapFile &heap_file,
                                       const BTreeIndex &index,
                                       std::size_t key_column_idx,
                                       std::string *error);

    static bool VerifyIndexAgainstHeap(const HeapFile &heap_file,
                                       const AbstractIndex &index,
                                       const std::vector<std::size_t> &key_column_indexes,
                                       std::string *error);

    static bool VerifyBTreeStructure(const BTreeIndex &index, std::string *error);
    static bool VerifyBTreeStructure(const GenericBTreeIndex &index, std::string *error);
};

}  // namespace simpledb
