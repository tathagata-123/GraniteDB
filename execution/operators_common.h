#pragma once

// Internal helper declarations shared by the split executor implementation files.
//
// The public executor API still lives in operators.h. This header only exposes
// low-level helper routines that were previously buried inside one very large
// operators.cpp file. Keeping them here makes the major operator families easy
// to navigate without changing runtime behavior.

#include "operators.h"

namespace simpledb {
namespace execution_detail {

int CompareValuesWithNulls(const Value &a, const Value &b);
int CompareTupleBySortKeys(const Tuple &a,
                          const Tuple &b,
                          const std::vector<SortKeySpec> &sort_keys,
                          const Schema &schema);
int CompareTuplesBySchema(const Tuple &a, const Tuple &b, const Schema &schema);
std::string MakeTempRunFileName();
uint64_t EncodeRid(const RID &rid);
RID DecodeRidTuple(const Tuple &tuple);
Tuple MakeRidTuple(const RID &rid);
bool CompareRids(const RID &a, const RID &b);
std::size_t EstimateValueSize(const Value &v);
std::size_t EstimateTupleSize(const Tuple &tuple);
void WriteTupleToStream(std::ofstream *out, const Tuple &tuple, const Schema &schema);
bool ReadTupleFromStream(std::ifstream *in, const Schema &schema, Tuple *tuple);

struct AggTransition {
    AggregateType type{AggregateType::COUNT};
    TypeId output_type{TypeId::INT64};
    bool initialized{false};
    int64_t count{0};
    int64_t sum_int{0};
    double sum_double{0.0};
    Value extreme = Value::Null(TypeId::INT64);
};

std::vector<TypeId> AggregateValueTypesFromSchema(const Schema &output_schema,
                                                  std::size_t group_count,
                                                  std::size_t agg_count);
std::vector<AggTransition> MakeInitialTransitions(const std::vector<AggregateType> &agg_types,
                                                  const std::vector<TypeId> &agg_value_types);
void ApplyAggregateRow(const Tuple &input,
                       const Schema &input_schema,
                       const std::vector<AggregateType> &agg_types,
                       const std::vector<std::unique_ptr<AbstractExpression>> &agg_exprs,
                       std::vector<AggTransition> *aggs);
Tuple BuildAggregateOutputTuple(const std::vector<Value> &group_vals,
                                const std::vector<AggTransition> &aggs,
                                const std::vector<TypeId> &agg_value_types);
void ApplyAggregateRowStreaming(const Tuple &input,
                                const Schema &input_schema,
                                const std::vector<AggregateType> &agg_types,
                                const std::vector<std::unique_ptr<AbstractExpression>> &agg_exprs,
                                std::vector<Value> *agg_values,
                                std::vector<bool> *agg_initialized,
                                const std::vector<TypeId> &agg_output_types);
Tuple BuildStreamingAggregateOutputTuple(const std::vector<Value> &group_vals,
                                         const std::vector<Value> &agg_values,
                                         const std::vector<bool> &agg_initialized,
                                         const std::vector<AggregateType> &agg_types,
                                         const std::vector<TypeId> &agg_output_types);
std::size_t NextPowerOfTwo(std::size_t x);

}  // namespace execution_detail
}  // namespace simpledb
