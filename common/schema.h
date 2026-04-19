#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "types.h"

namespace simpledb {

class Column {
public:
    Column(std::string name, TypeId type, bool nullable = true, uint32_t max_length = 0)
        : name_(std::move(name)),
          type_(type),
          nullable_(nullable),
          max_length_(max_length) {}

    const std::string &GetName() const { return name_; }
    TypeId GetType() const { return type_; }
    bool IsNullable() const { return nullable_; }
    uint32_t GetMaxLength() const { return max_length_; }
    bool IsInlined() const { return type_ != TypeId::VARCHAR; }

    uint32_t FixedLength() const {
        switch (type_) {
            case TypeId::BOOLEAN: return sizeof(bool);
            case TypeId::INT32: return sizeof(int32_t);
            case TypeId::INT64: return sizeof(int64_t);
            case TypeId::DOUBLE: return sizeof(double);
            case TypeId::VARCHAR: return 0;
            default: throw std::runtime_error("Invalid column type");
        }
    }

private:
    std::string name_;
    TypeId type_;
    bool nullable_;
    uint32_t max_length_;
};

class Schema {
public:
    Schema() = default;
    explicit Schema(std::vector<Column> columns) : columns_(std::move(columns)) {}

    const Column &GetColumn(std::size_t index) const {
        if (index >= columns_.size()) {
            throw std::out_of_range("Column index out of range");
        }
        return columns_[index];
    }

    std::size_t GetColumnCount() const { return columns_.size(); }
    const std::vector<Column> &GetColumns() const { return columns_; }

private:
    std::vector<Column> columns_;
};

}  // namespace simpledb
