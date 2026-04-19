// Shared executor helper routines.
//
// Nothing in this file changes semantics. It simply centralizes the small
// reusable building blocks that many executors need: value comparison rules,
// RID encoding helpers, temp-file tuple IO, and aggregate state transitions.
// Splitting these helpers out keeps the concrete operator files focused on one
// operator family at a time.

#include "operators_common.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <limits>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace simpledb {
namespace execution_detail {
int CompareValuesWithNulls(const Value &a, const Value &b) {
    if (a.IsNull() && b.IsNull()) return 0;
    if (a.IsNull()) return -1;
    if (b.IsNull()) return 1;
    return CompareValues(a, b);
}

int CompareTupleBySortKeys(const Tuple &a,
                          const Tuple &b,
                          const std::vector<SortKeySpec> &sort_keys,
                          const Schema &schema) {
    for (const auto &spec : sort_keys) {
        Value va = spec.expr->Evaluate(&a, &schema, nullptr, nullptr);
        Value vb = spec.expr->Evaluate(&b, &schema, nullptr, nullptr);
        int cmp = CompareValuesWithNulls(va, vb);
        if (cmp == 0) continue;
        if (!spec.ascending) cmp = -cmp;
        return cmp;
    }
    return 0;
}

int CompareTuplesBySchema(const Tuple &a, const Tuple &b, const Schema &schema) {
    for (std::size_t i = 0; i < schema.GetColumnCount(); i++) {
        int cmp = CompareValuesWithNulls(a.GetValue(i), b.GetValue(i));
        if (cmp != 0) return cmp;
    }
    return 0;
}

std::string MakeTempRunFileName() {
    static std::size_t counter = 0;
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path dir = std::filesystem::temp_directory_path();
    std::ostringstream name;
    name << "simpledb_tmp_" << now << "_" << counter++ << ".bin";
    return (dir / name.str()).string();
}

uint64_t EncodeRid(const RID &rid) {
    return (static_cast<uint64_t>(rid.page_no) << 32) | static_cast<uint64_t>(rid.slot_no);
}

RID DecodeRidTuple(const Tuple &tuple) {
    return RID{static_cast<PageNo>(tuple.GetValue(0).AsInt32()), static_cast<SlotNo>(tuple.GetValue(1).AsInt32())};
}

Tuple MakeRidTuple(const RID &rid) {
    return Tuple({Value(static_cast<int32_t>(rid.page_no)), Value(static_cast<int32_t>(rid.slot_no))});
}

bool CompareRids(const RID &a, const RID &b) {
    if (a.page_no != b.page_no) return a.page_no < b.page_no;
    return a.slot_no < b.slot_no;
}

std::size_t EstimateValueSize(const Value &v) {
    if (v.IsNull()) return 8;
    switch (v.GetTypeId()) {
        case TypeId::BOOLEAN: return 8;
        case TypeId::INT32: return 8;
        case TypeId::INT64: return 8;
        case TypeId::DOUBLE: return 8;
        case TypeId::VARCHAR: return 8 + v.AsString().size();
        default: return 8;
    }
}

std::size_t EstimateTupleSize(const Tuple &tuple) {
    std::size_t total = 16;
    for (const auto &v : tuple.GetValues()) total += EstimateValueSize(v);
    return total;
}

void WriteTupleToStream(std::ofstream *out, const Tuple &tuple, const Schema &schema) {
    std::vector<char> bytes = tuple.Serialize(schema);
    uint32_t len = static_cast<uint32_t>(bytes.size());
    out->write(reinterpret_cast<const char *>(&len), sizeof(len));
    out->write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

bool ReadTupleFromStream(std::ifstream *in, const Schema &schema, Tuple *tuple) {
    uint32_t len = 0;
    in->read(reinterpret_cast<char *>(&len), sizeof(len));
    if (!(*in)) return false;
    std::vector<char> bytes(len);
    in->read(bytes.data(), static_cast<std::streamsize>(len));
    if (!(*in)) throw std::runtime_error("Failed while reading tuple from temp file");
    *tuple = Tuple::Deserialize(schema, bytes.data(), bytes.size());
    return true;
}


std::vector<TypeId> AggregateValueTypesFromSchema(const Schema &output_schema,
                                                  std::size_t group_count,
                                                  std::size_t agg_count) {
    std::vector<TypeId> types;
    types.reserve(agg_count);
    for (std::size_t i = 0; i < agg_count; i++) {
        types.push_back(output_schema.GetColumn(group_count + i).GetType());
    }
    return types;
}

std::vector<AggTransition> MakeInitialTransitions(const std::vector<AggregateType> &agg_types,
                                                  const std::vector<TypeId> &agg_value_types) {
    std::vector<AggTransition> out(agg_types.size());
    for (std::size_t i = 0; i < agg_types.size(); i++) {
        out[i].type = agg_types[i];
        out[i].output_type = agg_value_types[i];
        out[i].extreme = Value::Null(agg_value_types[i]);
    }
    return out;
}

void ApplyAggregateRow(const Tuple &input,
                       const Schema &input_schema,
                       const std::vector<AggregateType> &agg_types,
                       const std::vector<std::unique_ptr<AbstractExpression>> &agg_exprs,
                       std::vector<AggTransition> *aggs) {
    for (std::size_t i = 0; i < agg_types.size(); i++) {
        AggregateType agg_type = agg_types[i];
        if (agg_type == AggregateType::COUNT) {
            if (agg_exprs[i] == nullptr) {
                (*aggs)[i].initialized = true;
                (*aggs)[i].count++;
            } else {
                Value v = agg_exprs[i]->Evaluate(&input, &input_schema, nullptr, nullptr);
                if (!v.IsNull()) {
                    (*aggs)[i].initialized = true;
                    (*aggs)[i].count++;
                }
            }
            continue;
        }

        Value v = agg_exprs[i]->Evaluate(&input, &input_schema, nullptr, nullptr);
        if (v.IsNull()) continue;

        switch (agg_type) {
            case AggregateType::SUM: {
                if (!IsNumericType(v.GetTypeId())) throw std::runtime_error("SUM requires numeric input");
                (*aggs)[i].initialized = true;
                if ((*aggs)[i].output_type == TypeId::DOUBLE || v.GetTypeId() == TypeId::DOUBLE) {
                    (*aggs)[i].sum_double += ValueToDouble(v);
                } else {
                    (*aggs)[i].sum_int += ValueToInt64(v);
                }
                break;
            }
            case AggregateType::MIN:
                if (!(*aggs)[i].initialized || CompareValues(v, (*aggs)[i].extreme) < 0) {
                    (*aggs)[i].initialized = true;
                    (*aggs)[i].extreme = v;
                }
                break;
            case AggregateType::MAX:
                if (!(*aggs)[i].initialized || CompareValues(v, (*aggs)[i].extreme) > 0) {
                    (*aggs)[i].initialized = true;
                    (*aggs)[i].extreme = v;
                }
                break;
            case AggregateType::COUNT:
                break;
        }
    }
}

Tuple BuildAggregateOutputTuple(const std::vector<Value> &group_vals,
                                const std::vector<AggTransition> &aggs,
                                const std::vector<TypeId> &agg_value_types) {
    std::vector<Value> out_vals = group_vals;
    out_vals.reserve(group_vals.size() + aggs.size());
    for (std::size_t i = 0; i < aggs.size(); i++) {
        const auto &agg = aggs[i];
        if (!agg.initialized) {
            if (agg.type == AggregateType::COUNT) out_vals.push_back(Value(static_cast<int64_t>(0)));
            else out_vals.push_back(Value::Null(agg_value_types[i]));
            continue;
        }
        switch (agg.type) {
            case AggregateType::COUNT:
                out_vals.push_back(Value(agg.count));
                break;
            case AggregateType::SUM:
                if (agg.output_type == TypeId::DOUBLE) out_vals.push_back(Value(agg.sum_double));
                else out_vals.push_back(Value(agg.sum_int));
                break;
            case AggregateType::MIN:
            case AggregateType::MAX:
                out_vals.push_back(agg.extreme);
                break;
        }
    }
    return Tuple(std::move(out_vals));
}

void ApplyAggregateRowStreaming(const Tuple &input,
                                const Schema &input_schema,
                                const std::vector<AggregateType> &agg_types,
                                const std::vector<std::unique_ptr<AbstractExpression>> &agg_exprs,
                                std::vector<Value> *agg_values,
                                std::vector<bool> *agg_initialized,
                                const std::vector<TypeId> &agg_output_types) {
    for (std::size_t i = 0; i < agg_types.size(); i++) {
        if (agg_types[i] == AggregateType::COUNT) {
            if (agg_exprs[i] == nullptr) {
                if (!(*agg_initialized)[i]) {
                    (*agg_initialized)[i] = true;
                    (*agg_values)[i] = Value(static_cast<int64_t>(0));
                }
                (*agg_values)[i] = Value((*agg_values)[i].AsInt64() + 1);
            } else {
                Value v = agg_exprs[i]->Evaluate(&input, &input_schema, nullptr, nullptr);
                if (!v.IsNull()) {
                    if (!(*agg_initialized)[i]) {
                        (*agg_initialized)[i] = true;
                        (*agg_values)[i] = Value(static_cast<int64_t>(0));
                    }
                    (*agg_values)[i] = Value((*agg_values)[i].AsInt64() + 1);
                }
            }
            continue;
        }

        Value v = agg_exprs[i]->Evaluate(&input, &input_schema, nullptr, nullptr);
        if (v.IsNull()) continue;

        if (!(*agg_initialized)[i]) {
            (*agg_initialized)[i] = true;
            if (agg_types[i] == AggregateType::SUM) {
                if (agg_output_types[i] == TypeId::DOUBLE || v.GetTypeId() == TypeId::DOUBLE) {
                    (*agg_values)[i] = Value(ValueToDouble(v));
                } else {
                    (*agg_values)[i] = Value(ValueToInt64(v));
                }
            } else {
                (*agg_values)[i] = v;
            }
            continue;
        }

        switch (agg_types[i]) {
            case AggregateType::SUM:
                if (agg_output_types[i] == TypeId::DOUBLE || (*agg_values)[i].GetTypeId() == TypeId::DOUBLE || v.GetTypeId() == TypeId::DOUBLE) {
                    (*agg_values)[i] = Value(ValueToDouble((*agg_values)[i]) + ValueToDouble(v));
                } else {
                    (*agg_values)[i] = Value(ValueToInt64((*agg_values)[i]) + ValueToInt64(v));
                }
                break;
            case AggregateType::MIN:
                if (CompareValues(v, (*agg_values)[i]) < 0) (*agg_values)[i] = v;
                break;
            case AggregateType::MAX:
                if (CompareValues(v, (*agg_values)[i]) > 0) (*agg_values)[i] = v;
                break;
            case AggregateType::COUNT:
                break;
        }
    }
}

Tuple BuildStreamingAggregateOutputTuple(const std::vector<Value> &group_vals,
                                         const std::vector<Value> &agg_values,
                                         const std::vector<bool> &agg_initialized,
                                         const std::vector<AggregateType> &agg_types,
                                         const std::vector<TypeId> &agg_output_types) {
    std::vector<Value> out_vals = group_vals;
    out_vals.reserve(group_vals.size() + agg_values.size());
    for (std::size_t i = 0; i < agg_values.size(); i++) {
        if (agg_initialized[i]) out_vals.push_back(agg_values[i]);
        else if (agg_types[i] == AggregateType::COUNT) out_vals.push_back(Value(static_cast<int64_t>(0)));
        else out_vals.push_back(Value::Null(agg_output_types[i]));
    }
    return Tuple(std::move(out_vals));
}

std::size_t NextPowerOfTwo(std::size_t x) {
    if (x <= 1) return 1;
    std::size_t p = 1;
    while (p < x) p <<= 1;
    return p;
}


}  // namespace execution_detail

Schema ConcatSchemas(const Schema &left, const Schema &right) {
    std::vector<Column> cols;
    for (const auto &c : left.GetColumns()) cols.push_back(c);
    for (const auto &c : right.GetColumns()) cols.push_back(c);
    return Schema(cols);
}

Tuple ConcatTuples(const Tuple &left, const Tuple &right) {
    std::vector<Value> vals;
    vals.reserve(left.Size() + right.Size());
    for (const auto &v : left.GetValues()) vals.push_back(v);
    for (const auto &v : right.GetValues()) vals.push_back(v);
    return Tuple(std::move(vals));
}

}  // namespace simpledb
