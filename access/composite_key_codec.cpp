#include "composite_key_codec.h"

#include <cstring>
#include <stdexcept>
#include <string>

#include "../execution/expressions.h"

namespace simpledb {
namespace {

void AppendBytes(std::vector<char> *out, const void *src, std::size_t len) {
    const char *ptr = static_cast<const char *>(src);
    out->insert(out->end(), ptr, ptr + len);
}

void AppendU32(std::vector<char> *out, uint32_t v) {
    AppendBytes(out, &v, sizeof(v));
}

uint32_t ReadU32(const char *data, uint32_t len, uint32_t *pos) {
    if (*pos + sizeof(uint32_t) > len) {
        throw std::runtime_error("Composite key decode underflow");
    }
    uint32_t out = 0;
    std::memcpy(&out, data + *pos, sizeof(out));
    *pos += sizeof(out);
    return out;
}

uint32_t MaxEncodedValueSize(const IndexKeyColumnDefinition &col) {
    switch (col.type) {
        case TypeId::BOOLEAN: return 1 + sizeof(bool);
        case TypeId::INT32: return 1 + sizeof(int32_t);
        case TypeId::INT64: return 1 + sizeof(int64_t);
        case TypeId::DOUBLE: return 1 + sizeof(double);
        case TypeId::VARCHAR: return 1 + sizeof(uint32_t) + col.max_varchar_len;
        default:
            throw std::runtime_error("Unsupported composite key type");
    }
}

Value ReadValue(const char *data,
                uint32_t len,
                uint32_t *pos,
                const IndexKeyColumnDefinition &col) {
    if (*pos >= len) {
        throw std::runtime_error("Composite key decode underflow while reading null tag");
    }
    uint8_t is_null = static_cast<uint8_t>(data[*pos]);
    *pos += 1;
    if (is_null != 0) {
        return Value::Null(col.type);
    }

    switch (col.type) {
        case TypeId::BOOLEAN: {
            if (*pos + sizeof(bool) > len) throw std::runtime_error("Bad BOOLEAN composite key");
            bool x = false;
            std::memcpy(&x, data + *pos, sizeof(x));
            *pos += sizeof(x);
            return Value(x);
        }
        case TypeId::INT32: {
            if (*pos + sizeof(int32_t) > len) throw std::runtime_error("Bad INT32 composite key");
            int32_t x = 0;
            std::memcpy(&x, data + *pos, sizeof(x));
            *pos += sizeof(x);
            return Value(x);
        }
        case TypeId::INT64: {
            if (*pos + sizeof(int64_t) > len) throw std::runtime_error("Bad INT64 composite key");
            int64_t x = 0;
            std::memcpy(&x, data + *pos, sizeof(x));
            *pos += sizeof(x);
            return Value(x);
        }
        case TypeId::DOUBLE: {
            if (*pos + sizeof(double) > len) throw std::runtime_error("Bad DOUBLE composite key");
            double x = 0;
            std::memcpy(&x, data + *pos, sizeof(x));
            *pos += sizeof(x);
            return Value(x);
        }
        case TypeId::VARCHAR: {
            uint32_t slen = ReadU32(data, len, pos);
            if (*pos + slen > len) throw std::runtime_error("Bad VARCHAR composite key");
            std::string s(data + *pos, data + *pos + slen);
            *pos += slen;
            return Value(s);
        }
        default:
            throw std::runtime_error("Unsupported composite key type");
    }
}

int CompareWithNullPolicy(const Value &lhs, const Value &rhs, NullPolicy policy) {
    if (!lhs.IsNull() && !rhs.IsNull()) {
        return CompareValues(lhs, rhs);
    }
    if (lhs.IsNull() && rhs.IsNull()) {
        return 0;
    }
    if (policy == NullPolicy::NULLS_LOW) {
        return lhs.IsNull() ? -1 : 1;
    }
    if (policy == NullPolicy::NULLS_HIGH) {
        return lhs.IsNull() ? 1 : -1;
    }
    throw std::runtime_error("NULL encountered for composite key under unsupported null policy");
}

}  // namespace

uint32_t CompositeKeyCodec::GetMaxEncodedKeySize(const std::vector<IndexKeyColumnDefinition> &definition) {
    uint32_t total = 0;
    for (const auto &col : definition) total += MaxEncodedValueSize(col);
    return total;
}

std::vector<char> CompositeKeyCodec::EncodeKey(const std::vector<Value> &values,
                                               const std::vector<IndexKeyColumnDefinition> &definition,
                                               NullPolicy null_policy) {
    if (values.size() != definition.size()) {
        throw std::runtime_error("CompositeKeyCodec value/definition length mismatch");
    }

    std::vector<char> out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const Value &value = values[i];
        const auto &col = definition[i];
        if (value.IsNull()) {
            if (null_policy == NullPolicy::NOT_SUPPORTED) {
                throw std::runtime_error("NULL composite key component is not supported by this index");
            }
            if (!col.nullable) {
                throw std::runtime_error("NULL composite key component for non-nullable column");
            }
            out.push_back(1);
            continue;
        }

        if (value.GetTypeId() != col.type) {
            throw std::runtime_error("Composite key type mismatch");
        }
        out.push_back(0);

        switch (col.type) {
            case TypeId::BOOLEAN: {
                bool x = value.AsBool();
                AppendBytes(&out, &x, sizeof(x));
                break;
            }
            case TypeId::INT32: {
                int32_t x = value.AsInt32();
                AppendBytes(&out, &x, sizeof(x));
                break;
            }
            case TypeId::INT64: {
                int64_t x = value.AsInt64();
                AppendBytes(&out, &x, sizeof(x));
                break;
            }
            case TypeId::DOUBLE: {
                double x = value.AsDouble();
                AppendBytes(&out, &x, sizeof(x));
                break;
            }
            case TypeId::VARCHAR: {
                const std::string &s = value.AsString();
                if (col.max_varchar_len > 0 && s.size() > col.max_varchar_len) {
                    throw std::runtime_error("VARCHAR composite key component exceeds max length");
                }
                uint32_t slen = static_cast<uint32_t>(s.size());
                AppendU32(&out, slen);
                AppendBytes(&out, s.data(), slen);
                break;
            }
            default:
                throw std::runtime_error("Unsupported composite key type");
        }
    }
    return out;
}

int CompositeKeyCodec::CompareEncoded(const std::vector<char> &lhs,
                                      const std::vector<char> &rhs,
                                      const std::vector<IndexKeyColumnDefinition> &definition,
                                      NullPolicy null_policy) {
    return CompareEncoded(lhs.data(),
                          static_cast<uint32_t>(lhs.size()),
                          rhs.data(),
                          static_cast<uint32_t>(rhs.size()),
                          definition,
                          null_policy);
}

int CompositeKeyCodec::CompareEncoded(const char *lhs,
                                      uint32_t lhs_len,
                                      const char *rhs,
                                      uint32_t rhs_len,
                                      const std::vector<IndexKeyColumnDefinition> &definition,
                                      NullPolicy null_policy) {
    uint32_t lhs_pos = 0;
    uint32_t rhs_pos = 0;
    for (const auto &col : definition) {
        Value lhs_value = ReadValue(lhs, lhs_len, &lhs_pos, col);
        Value rhs_value = ReadValue(rhs, rhs_len, &rhs_pos, col);
        int cmp = CompareWithNullPolicy(lhs_value, rhs_value, null_policy);
        if (cmp != 0) {
            return cmp;
        }
    }
    if (lhs_pos != lhs_len || rhs_pos != rhs_len) {
        throw std::runtime_error("Composite key payload length mismatch");
    }
    return 0;
}

std::vector<Value> CompositeKeyCodec::DecodeKey(const std::vector<char> &encoded,
                                                const std::vector<IndexKeyColumnDefinition> &definition) {
    std::vector<Value> out;
    out.reserve(definition.size());
    uint32_t pos = 0;
    for (const auto &col : definition) {
        out.push_back(ReadValue(encoded.data(), static_cast<uint32_t>(encoded.size()), &pos, col));
    }
    if (pos != encoded.size()) {
        throw std::runtime_error("Composite key payload length mismatch while decoding key");
    }
    return out;
}

}  // namespace simpledb
