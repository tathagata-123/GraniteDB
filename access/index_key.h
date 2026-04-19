#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "../common/types.h"
#include "../common/value.h"

namespace simpledb {

class IndexKeyUtil {
public:
    static uint32_t MaxEncodedKeySize(TypeId key_type, uint32_t max_varchar_len) {
        switch (key_type) {
            case TypeId::BOOLEAN: return sizeof(bool);
            case TypeId::INT32: return sizeof(int32_t);
            case TypeId::INT64: return sizeof(int64_t);
            case TypeId::DOUBLE: return sizeof(double);
            case TypeId::VARCHAR: return sizeof(uint32_t) + max_varchar_len;
            default: throw std::runtime_error("Unsupported index key type");
        }
    }

    static std::vector<char> EncodeValue(const Value &value,
                                         TypeId key_type,
                                         uint32_t max_varchar_len) {
        if (value.IsNull()) {
            throw std::runtime_error("NULL keys are not supported in this B+ tree");
        }
        if (value.GetTypeId() != key_type) {
            throw std::runtime_error("Index key type mismatch");
        }

        std::vector<char> out;
        switch (key_type) {
            case TypeId::BOOLEAN: {
                bool x = value.AsBool();
                out.resize(sizeof(x));
                std::memcpy(out.data(), &x, sizeof(x));
                return out;
            }
            case TypeId::INT32: {
                int32_t x = value.AsInt32();
                out.resize(sizeof(x));
                std::memcpy(out.data(), &x, sizeof(x));
                return out;
            }
            case TypeId::INT64: {
                int64_t x = value.AsInt64();
                out.resize(sizeof(x));
                std::memcpy(out.data(), &x, sizeof(x));
                return out;
            }
            case TypeId::DOUBLE: {
                double x = value.AsDouble();
                out.resize(sizeof(x));
                std::memcpy(out.data(), &x, sizeof(x));
                return out;
            }
            case TypeId::VARCHAR: {
                const std::string &s = value.AsString();
                if (s.size() > max_varchar_len) {
                    throw std::runtime_error("VARCHAR key exceeds declared max length");
                }
                uint32_t len = static_cast<uint32_t>(s.size());
                out.resize(sizeof(len) + len);
                std::memcpy(out.data(), &len, sizeof(len));
                std::memcpy(out.data() + sizeof(len), s.data(), len);
                return out;
            }
            default:
                throw std::runtime_error("Unsupported index key type");
        }
    }

    static Value DecodeValue(TypeId key_type, const char *data, uint32_t len) {
        switch (key_type) {
            case TypeId::BOOLEAN: {
                if (len != sizeof(bool)) throw std::runtime_error("Bad BOOLEAN key length");
                bool x; std::memcpy(&x, data, sizeof(x)); return Value(x);
            }
            case TypeId::INT32: {
                if (len != sizeof(int32_t)) throw std::runtime_error("Bad INT32 key length");
                int32_t x; std::memcpy(&x, data, sizeof(x)); return Value(x);
            }
            case TypeId::INT64: {
                if (len != sizeof(int64_t)) throw std::runtime_error("Bad INT64 key length");
                int64_t x; std::memcpy(&x, data, sizeof(x)); return Value(x);
            }
            case TypeId::DOUBLE: {
                if (len != sizeof(double)) throw std::runtime_error("Bad DOUBLE key length");
                double x; std::memcpy(&x, data, sizeof(x)); return Value(x);
            }
            case TypeId::VARCHAR: {
                if (len < sizeof(uint32_t)) throw std::runtime_error("Bad VARCHAR key length");
                uint32_t slen; std::memcpy(&slen, data, sizeof(slen));
                if (sizeof(uint32_t) + slen != len) throw std::runtime_error("Corrupt VARCHAR key");
                std::string s(data + sizeof(uint32_t), data + sizeof(uint32_t) + slen);
                return Value(s);
            }
            default:
                throw std::runtime_error("Unsupported index key type");
        }
    }

    static int CompareEncoded(TypeId key_type,
                              const std::vector<char> &a,
                              const std::vector<char> &b) {
        return CompareEncoded(key_type,
                              a.data(), static_cast<uint32_t>(a.size()),
                              b.data(), static_cast<uint32_t>(b.size()));
    }

    static int CompareEncoded(TypeId key_type,
                              const char *a, uint32_t a_len,
                              const char *b, uint32_t b_len) {
        switch (key_type) {
            case TypeId::BOOLEAN: {
                if (a_len != sizeof(bool) || b_len != sizeof(bool)) {
                    throw std::runtime_error("Bad BOOLEAN key length");
                }
                bool x, y;
                std::memcpy(&x, a, sizeof(x));
                std::memcpy(&y, b, sizeof(y));
                if (x < y) return -1;
                if (x > y) return 1;
                return 0;
            }
            case TypeId::INT32: {
                if (a_len != sizeof(int32_t) || b_len != sizeof(int32_t)) {
                    throw std::runtime_error("Bad INT32 key length");
                }
                int32_t x, y;
                std::memcpy(&x, a, sizeof(x));
                std::memcpy(&y, b, sizeof(y));
                if (x < y) return -1;
                if (x > y) return 1;
                return 0;
            }
            case TypeId::INT64: {
                if (a_len != sizeof(int64_t) || b_len != sizeof(int64_t)) {
                    throw std::runtime_error("Bad INT64 key length");
                }
                int64_t x, y;
                std::memcpy(&x, a, sizeof(x));
                std::memcpy(&y, b, sizeof(y));
                if (x < y) return -1;
                if (x > y) return 1;
                return 0;
            }
            case TypeId::DOUBLE: {
                if (a_len != sizeof(double) || b_len != sizeof(double)) {
                    throw std::runtime_error("Bad DOUBLE key length");
                }
                double x, y;
                std::memcpy(&x, a, sizeof(x));
                std::memcpy(&y, b, sizeof(y));
                if (x < y) return -1;
                if (x > y) return 1;
                return 0;
            }
            case TypeId::VARCHAR: {
                if (a_len < sizeof(uint32_t) || b_len < sizeof(uint32_t)) {
                    throw std::runtime_error("Bad VARCHAR key length");
                }
                uint32_t a_slen, b_slen;
                std::memcpy(&a_slen, a, sizeof(a_slen));
                std::memcpy(&b_slen, b, sizeof(b_slen));
                if (sizeof(uint32_t) + a_slen != a_len ||
                    sizeof(uint32_t) + b_slen != b_len) {
                    throw std::runtime_error("Corrupt VARCHAR key");
                }
                std::string sa(a + sizeof(uint32_t), a + sizeof(uint32_t) + a_slen);
                std::string sb(b + sizeof(uint32_t), b + sizeof(uint32_t) + b_slen);
                if (sa < sb) return -1;
                if (sa > sb) return 1;
                return 0;
            }
            default:
                throw std::runtime_error("Unsupported index key type");
        }
    }
};

}  // namespace simpledb
