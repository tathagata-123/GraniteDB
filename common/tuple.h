#pragma once

#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "schema.h"
#include "value.h"

namespace simpledb {

class Tuple {
public:
    Tuple() = default;
    explicit Tuple(std::vector<Value> values) : values_(std::move(values)) {}

    const std::vector<Value> &GetValues() const { return values_; }

    const Value &GetValue(std::size_t index) const {
        if (index >= values_.size()) {
            throw std::out_of_range("Tuple value index out of range");
        }
        return values_[index];
    }

    std::size_t Size() const { return values_.size(); }

    std::vector<char> Serialize(const Schema &schema) const {
        ValidateAgainstSchema(schema);

        std::vector<char> out;

        auto write_bytes = [&](const void *src, std::size_t len) {
            const char *p = static_cast<const char *>(src);
            out.insert(out.end(), p, p + len);
        };
        auto write_u16 = [&](uint16_t v) { write_bytes(&v, sizeof(v)); };
        auto write_u32 = [&](uint32_t v) { write_bytes(&v, sizeof(v)); };

        uint16_t column_count = static_cast<uint16_t>(values_.size());
        write_u16(column_count);

        std::size_t bitmap_bytes = (column_count + 7) / 8;
        std::size_t bitmap_start = out.size();
        out.resize(out.size() + bitmap_bytes, 0);

        for (std::size_t i = 0; i < values_.size(); i++) {
            if (values_[i].IsNull()) {
                out[bitmap_start + i / 8] |= (1u << (i % 8));
            }
        }

        for (std::size_t i = 0; i < values_.size(); i++) {
            const Column &col = schema.GetColumn(i);
            const Value &val = values_[i];

            if (val.IsNull()) {
                continue;
            }

            switch (col.GetType()) {
                case TypeId::BOOLEAN: {
                    bool x = val.AsBool();
                    write_bytes(&x, sizeof(x));
                    break;
                }
                case TypeId::INT32: {
                    int32_t x = val.AsInt32();
                    write_bytes(&x, sizeof(x));
                    break;
                }
                case TypeId::INT64: {
                    int64_t x = val.AsInt64();
                    write_bytes(&x, sizeof(x));
                    break;
                }
                case TypeId::DOUBLE: {
                    double x = val.AsDouble();
                    write_bytes(&x, sizeof(x));
                    break;
                }
                case TypeId::VARCHAR: {
                    const std::string &s = val.AsString();
                    uint32_t len = static_cast<uint32_t>(s.size());
                    write_u32(len);
                    write_bytes(s.data(), len);
                    break;
                }
                default:
                    throw std::runtime_error("Unsupported type during serialization");
            }
        }

        return out;
    }

    static Tuple Deserialize(const Schema &schema, const char *data, std::size_t len) {
        std::size_t pos = 0;

        auto read_bytes = [&](void *dst, std::size_t n) {
            if (pos + n > len) {
                throw std::runtime_error("Tuple deserialize: buffer underflow");
            }
            std::memcpy(dst, data + pos, n);
            pos += n;
        };

        auto read_u16 = [&]() -> uint16_t {
            uint16_t x;
            read_bytes(&x, sizeof(x));
            return x;
        };
        auto read_u32 = [&]() -> uint32_t {
            uint32_t x;
            read_bytes(&x, sizeof(x));
            return x;
        };

        uint16_t column_count = read_u16();
        if (column_count != schema.GetColumnCount()) {
            throw std::runtime_error("Tuple deserialize: schema column count mismatch");
        }

        std::size_t bitmap_bytes = (column_count + 7) / 8;
        std::vector<unsigned char> bitmap(bitmap_bytes);
        read_bytes(bitmap.data(), bitmap_bytes);

        std::vector<Value> values;
        values.reserve(column_count);

        for (std::size_t i = 0; i < column_count; i++) {
            const Column &col = schema.GetColumn(i);
            bool is_null = (bitmap[i / 8] >> (i % 8)) & 1u;

            if (is_null) {
                values.push_back(Value::Null(col.GetType()));
                continue;
            }

            switch (col.GetType()) {
                case TypeId::BOOLEAN: {
                    bool x;
                    read_bytes(&x, sizeof(x));
                    values.emplace_back(x);
                    break;
                }
                case TypeId::INT32: {
                    int32_t x;
                    read_bytes(&x, sizeof(x));
                    values.emplace_back(x);
                    break;
                }
                case TypeId::INT64: {
                    int64_t x;
                    read_bytes(&x, sizeof(x));
                    values.emplace_back(x);
                    break;
                }
                case TypeId::DOUBLE: {
                    double x;
                    read_bytes(&x, sizeof(x));
                    values.emplace_back(x);
                    break;
                }
                case TypeId::VARCHAR: {
                    uint32_t slen = read_u32();
                    if (pos + slen > len) {
                        throw std::runtime_error("Tuple deserialize: bad string length");
                    }
                    std::string s(data + pos, data + pos + slen);
                    pos += slen;
                    values.emplace_back(s);
                    break;
                }
                default:
                    throw std::runtime_error("Unsupported type during deserialization");
            }
        }

        return Tuple(values);
    }

    std::string ToString(const Schema &schema) const {
        if (values_.size() != schema.GetColumnCount()) {
            throw std::runtime_error("Tuple::ToString schema mismatch");
        }

        std::ostringstream out;
        out << "(";
        for (std::size_t i = 0; i < values_.size(); i++) {
            out << schema.GetColumn(i).GetName() << "=" << values_[i].ToString();
            if (i + 1 < values_.size()) {
                out << ", ";
            }
        }
        out << ")";
        return out.str();
    }

private:
    void ValidateAgainstSchema(const Schema &schema) const {
        if (values_.size() != schema.GetColumnCount()) {
            throw std::runtime_error("Tuple/schema column count mismatch");
        }

        for (std::size_t i = 0; i < values_.size(); i++) {
            const Column &col = schema.GetColumn(i);
            const Value &val = values_[i];

            if (val.IsNull()) {
                if (!col.IsNullable()) {
                    throw std::runtime_error("NULL inserted into non-nullable column: " + col.GetName());
                }
                continue;
            }

            if (val.GetTypeId() != col.GetType()) {
                throw std::runtime_error("Type mismatch for column: " + col.GetName());
            }

            if (col.GetType() == TypeId::VARCHAR) {
                const std::string &s = val.AsString();
                if (col.GetMaxLength() > 0 && s.size() > col.GetMaxLength()) {
                    throw std::runtime_error("VARCHAR too long for column: " + col.GetName());
                }
            }
        }
    }

private:
    std::vector<Value> values_;
};

}  // namespace simpledb
