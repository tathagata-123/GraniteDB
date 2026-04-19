#pragma once

#include "../common/schema.h"
#include "../common/tuple.h"

namespace simpledb {

class AbstractExecutor {
public:
    virtual ~AbstractExecutor() = default;

    virtual void Init() = 0;
    virtual bool Next(Tuple *out_tuple) = 0;
    virtual void Close() = 0;

    virtual const Schema &GetOutputSchema() const = 0;

    virtual bool SupportsMarkRestore() const { return false; }
    virtual void MarkPosition() {}
    virtual void RestorePosition() {}
};

}  // namespace simpledb
