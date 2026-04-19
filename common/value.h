#pragma once

#include <cassert>
#include <cstdint>
#include <sstream>
#include <string>
#include <variant>

#include "types.h"

namespace simpledb {

class Value {
public:
    Value() = default;

    static Value Null(TypeId type) {
        Value v;
        v.type_ = type;
        v.is_null_ = true;
        v.data_ = std::monostate{};
        return v;
    }

    explicit Value(bool v) : type_(TypeId::BOOLEAN), is_null_(false), data_(v) {}
    explicit Value(int32_t v) : type_(TypeId::INT32), is_null_(false), data_(v) {}
    explicit Value(int64_t v) : type_(TypeId::INT64), is_null_(false), data_(v) {}
    explicit Value(double v) : type_(TypeId::DOUBLE), is_null_(false), data_(v) {}
    explicit Value(const std::string &v) : type_(TypeId::VARCHAR), is_null_(false), data_(v) {}
    explicit Value(const char *v) : type_(TypeId::VARCHAR), is_null_(false), data_(std::string(v)) {}

    TypeId GetTypeId() const { return type_; }
    bool IsNull() const { return is_null_; }

    bool AsBool() const {
        assert(type_ == TypeId::BOOLEAN && !is_null_);
        return std::get<bool>(data_);
    }
    int32_t AsInt32() const {
        assert(type_ == TypeId::INT32 && !is_null_);
        return std::get<int32_t>(data_);
    }
    int64_t AsInt64() const {
        assert(type_ == TypeId::INT64 && !is_null_);
        return std::get<int64_t>(data_);
    }
    double AsDouble() const {
        assert(type_ == TypeId::DOUBLE && !is_null_);
        return std::get<double>(data_);
    }
    const std::string &AsString() const {
        assert(type_ == TypeId::VARCHAR && !is_null_);
        return std::get<std::string>(data_);
    }

    std::string ToString() const {
        if (is_null_) return "NULL";
        std::ostringstream out;
        switch (type_) {
            case TypeId::BOOLEAN: return AsBool() ? "true" : "false";
            case TypeId::INT32: out << AsInt32(); return out.str();
            case TypeId::INT64: out << AsInt64(); return out.str();
            case TypeId::DOUBLE: out << AsDouble(); return out.str();
            case TypeId::VARCHAR: return AsString();
            default: return "INVALID";
        }
    }

private:
    TypeId type_{TypeId::INVALID};
    bool is_null_{true};
    std::variant<std::monostate, bool, int32_t, int64_t, double, std::string> data_;
};

}  // namespace simpledb
