#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "../common/schema.h"
#include "../common/tuple.h"
#include "../common/value.h"

namespace simpledb {

struct HeapTupleHeader {
    uint32_t total_length;
    uint16_t null_bitmap_offset;
    uint16_t column_count;
    uint8_t deleted;
    uint8_t reserved[3];
};

static_assert(sizeof(HeapTupleHeader) == 12, "Unexpected HeapTupleHeader size");

class HeapTupleCodec {
public:
    static std::vector<char> Encode(const Tuple &tuple, const Schema &schema) {
        ValidateTupleAgainstSchema(tuple, schema);

        const uint16_t column_count = static_cast<uint16_t>(schema.GetColumnCount());
        const uint16_t bitmap_bytes = static_cast<uint16_t>((column_count + 7) / 8);

        std::size_t payload_bytes = 0;
        for (std::size_t i = 0; i < tuple.Size(); i++) {
            const Column &col = schema.GetColumn(i);
            const Value &val = tuple.GetValue(i);

            if (val.IsNull()) {
                continue;
            }

            switch (col.GetType()) {
                case TypeId::BOOLEAN: payload_bytes += sizeof(bool); break;
                case TypeId::INT32: payload_bytes += sizeof(int32_t); break;
                case TypeId::INT64: payload_bytes += sizeof(int64_t); break;
                case TypeId::DOUBLE: payload_bytes += sizeof(double); break;
                case TypeId::VARCHAR: payload_bytes += sizeof(uint32_t) + val.AsString().size(); break;
                default: throw std::runtime_error("Unsupported type in HeapTupleCodec::Encode");
            }
        }

        const std::size_t total_len = sizeof(HeapTupleHeader) + bitmap_bytes + payload_bytes;
        if (total_len > UINT32_MAX) {
            throw std::runtime_error("Tuple too large");
        }

        std::vector<char> out(total_len, 0);

        HeapTupleHeader hdr{};
        hdr.total_length = static_cast<uint32_t>(total_len);
        hdr.null_bitmap_offset = sizeof(HeapTupleHeader);
        hdr.column_count = column_count;
        hdr.deleted = 0;
        std::memcpy(out.data(), &hdr, sizeof(hdr));

        unsigned char *bitmap =
            reinterpret_cast<unsigned char *>(out.data() + hdr.null_bitmap_offset);

        std::size_t write_pos = sizeof(HeapTupleHeader) + bitmap_bytes;

        for (std::size_t i = 0; i < tuple.Size(); i++) {
            const Column &col = schema.GetColumn(i);
            const Value &val = tuple.GetValue(i);

            if (val.IsNull()) {
                bitmap[i / 8] |= static_cast<unsigned char>(1u << (i % 8));
                continue;
            }

            switch (col.GetType()) {
                case TypeId::BOOLEAN: {
                    bool x = val.AsBool();
                    std::memcpy(out.data() + write_pos, &x, sizeof(x));
                    write_pos += sizeof(x);
                    break;
                }
                case TypeId::INT32: {
                    int32_t x = val.AsInt32();
                    std::memcpy(out.data() + write_pos, &x, sizeof(x));
                    write_pos += sizeof(x);
                    break;
                }
                case TypeId::INT64: {
                    int64_t x = val.AsInt64();
                    std::memcpy(out.data() + write_pos, &x, sizeof(x));
                    write_pos += sizeof(x);
                    break;
                }
                case TypeId::DOUBLE: {
                    double x = val.AsDouble();
                    std::memcpy(out.data() + write_pos, &x, sizeof(x));
                    write_pos += sizeof(x);
                    break;
                }
                case TypeId::VARCHAR: {
                    const std::string &s = val.AsString();
                    uint32_t len = static_cast<uint32_t>(s.size());
                    std::memcpy(out.data() + write_pos, &len, sizeof(len));
                    write_pos += sizeof(len);
                    std::memcpy(out.data() + write_pos, s.data(), s.size());
                    write_pos += s.size();
                    break;
                }
                default:
                    throw std::runtime_error("Unsupported type in HeapTupleCodec::Encode");
            }
        }

        return out;
    }

    static Tuple Decode(const Schema &schema, const char *data, std::size_t len) {
        if (len < sizeof(HeapTupleHeader)) {
            throw std::runtime_error("Heap tuple too small");
        }

        HeapTupleHeader hdr = ReadHeader(data, len);

        if (hdr.total_length > len) {
            throw std::runtime_error("Heap tuple length exceeds available bytes");
        }

        if (hdr.column_count != schema.GetColumnCount()) {
            throw std::runtime_error("Heap tuple/schema column count mismatch");
        }

        const std::size_t bitmap_bytes = (hdr.column_count + 7) / 8;
        if (hdr.null_bitmap_offset + bitmap_bytes > hdr.total_length) {
            throw std::runtime_error("Corrupt null bitmap");
        }

        const unsigned char *bitmap =
            reinterpret_cast<const unsigned char *>(data + hdr.null_bitmap_offset);

        std::size_t read_pos = sizeof(HeapTupleHeader) + bitmap_bytes;
        std::vector<Value> values;
        values.reserve(hdr.column_count);

        for (std::size_t i = 0; i < hdr.column_count; i++) {
            const Column &col = schema.GetColumn(i);
            bool is_null = (bitmap[i / 8] >> (i % 8)) & 1u;

            if (is_null) {
                values.push_back(Value::Null(col.GetType()));
                continue;
            }

            switch (col.GetType()) {
                case TypeId::BOOLEAN: {
                    EnsureAvailable(read_pos, sizeof(bool), hdr.total_length);
                    bool x;
                    std::memcpy(&x, data + read_pos, sizeof(x));
                    read_pos += sizeof(x);
                    values.emplace_back(x);
                    break;
                }
                case TypeId::INT32: {
                    EnsureAvailable(read_pos, sizeof(int32_t), hdr.total_length);
                    int32_t x;
                    std::memcpy(&x, data + read_pos, sizeof(x));
                    read_pos += sizeof(x);
                    values.emplace_back(x);
                    break;
                }
                case TypeId::INT64: {
                    EnsureAvailable(read_pos, sizeof(int64_t), hdr.total_length);
                    int64_t x;
                    std::memcpy(&x, data + read_pos, sizeof(x));
                    read_pos += sizeof(x);
                    values.emplace_back(x);
                    break;
                }
                case TypeId::DOUBLE: {
                    EnsureAvailable(read_pos, sizeof(double), hdr.total_length);
                    double x;
                    std::memcpy(&x, data + read_pos, sizeof(x));
                    read_pos += sizeof(x);
                    values.emplace_back(x);
                    break;
                }
                case TypeId::VARCHAR: {
                    EnsureAvailable(read_pos, sizeof(uint32_t), hdr.total_length);
                    uint32_t slen;
                    std::memcpy(&slen, data + read_pos, sizeof(slen));
                    read_pos += sizeof(slen);

                    EnsureAvailable(read_pos, slen, hdr.total_length);
                    std::string s(data + read_pos, data + read_pos + slen);
                    read_pos += slen;
                    values.emplace_back(s);
                    break;
                }
                default:
                    throw std::runtime_error("Unsupported type in HeapTupleCodec::Decode");
            }
        }

        return Tuple(values);
    }

    static uint32_t GetStoredLength(const char *data, std::size_t len) {
        HeapTupleHeader hdr = ReadHeader(data, len);
        return hdr.total_length;
    }

    static bool IsDeleted(const char *data, std::size_t len) {
        HeapTupleHeader hdr = ReadHeader(data, len);
        return hdr.deleted != 0;
    }

    static void MarkDeleted(char *data, std::size_t len) {
        HeapTupleHeader hdr = ReadHeader(data, len);
        hdr.deleted = 1;
        std::memcpy(data, &hdr, sizeof(hdr));
    }

private:
    static void EnsureAvailable(std::size_t pos, std::size_t need, std::size_t total) {
        if (pos + need > total) {
            throw std::runtime_error("Heap tuple decode overflow");
        }
    }

    static HeapTupleHeader ReadHeader(const char *data, std::size_t len) {
        if (len < sizeof(HeapTupleHeader)) {
            throw std::runtime_error("Heap tuple too small for header");
        }

        HeapTupleHeader hdr{};
        std::memcpy(&hdr, data, sizeof(hdr));
        if (hdr.total_length < sizeof(HeapTupleHeader)) {
            throw std::runtime_error("Corrupt heap tuple header");
        }
        return hdr;
    }

    static void ValidateTupleAgainstSchema(const Tuple &tuple, const Schema &schema) {
        if (tuple.Size() != schema.GetColumnCount()) {
            throw std::runtime_error("Tuple/schema column count mismatch");
        }

        for (std::size_t i = 0; i < tuple.Size(); i++) {
            const Column &col = schema.GetColumn(i);
            const Value &val = tuple.GetValue(i);

            if (val.IsNull()) {
                if (!col.IsNullable()) {
                    throw std::runtime_error("NULL into non-nullable column: " + col.GetName());
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
};

}  // namespace simpledb
