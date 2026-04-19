#pragma once

#include <cstdint>
#include <vector>

#include "index.h"

namespace simpledb {

class CompositeKeyCodec {
public:
    static uint32_t GetMaxEncodedKeySize(const std::vector<IndexKeyColumnDefinition> &definition);

    static std::vector<char> EncodeKey(const std::vector<Value> &values,
                                       const std::vector<IndexKeyColumnDefinition> &definition,
                                       NullPolicy null_policy = NullPolicy::NOT_SUPPORTED);

    static int CompareEncoded(const std::vector<char> &lhs,
                              const std::vector<char> &rhs,
                              const std::vector<IndexKeyColumnDefinition> &definition,
                              NullPolicy null_policy = NullPolicy::NOT_SUPPORTED);

    static std::vector<Value> DecodeKey(const std::vector<char> &encoded,
                                        const std::vector<IndexKeyColumnDefinition> &definition);

private:
    static int CompareEncoded(const char *lhs,
                              uint32_t lhs_len,
                              const char *rhs,
                              uint32_t rhs_len,
                              const std::vector<IndexKeyColumnDefinition> &definition,
                              NullPolicy null_policy);
};

}  // namespace simpledb
