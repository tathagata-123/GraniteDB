// Hash-style and streaming aggregation executors.

#include "operators_common.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace simpledb {
using namespace execution_detail;

AggregateExecutor::AggregateExecutor(std::unique_ptr<AbstractExecutor> child,
                                     std::vector<std::unique_ptr<AbstractExpression>> group_by_exprs,
                                     std::vector<AggregateType> agg_types,
                                     std::vector<std::unique_ptr<AbstractExpression>> agg_exprs,
                                     Schema output_schema,
                                     std::size_t memory_budget_bytes)
    : child_(std::move(child)), group_by_exprs_(std::move(group_by_exprs)), agg_types_(std::move(agg_types)),
      agg_exprs_(std::move(agg_exprs)), output_schema_(std::move(output_schema)), memory_budget_bytes_(memory_budget_bytes),
      pos_(0), current_batch_idx_(0), emit_empty_(false) {
    if (agg_types_.size() != agg_exprs_.size()) {
        throw std::runtime_error("Aggregate types and aggregate expressions size mismatch");
    }
}

std::string AggregateExecutor::MaterializeChildInput(std::size_t *bytes_written) {
    std::string file_name = MakeTempRunFileName();
    std::ofstream out(file_name, std::ios::binary);
    if (!out) throw std::runtime_error("Failed to create aggregate temp file");
    *bytes_written = 0;
    Tuple tuple;
    while (child_->Next(&tuple)) {
        WriteTupleToStream(&out, tuple, child_->GetOutputSchema());
        *bytes_written += EstimateTupleSize(tuple);
    }
    out.close();
    return file_name;
}

void AggregateExecutor::RemoveBatchFiles() {
    for (const auto &batch : batch_files_) {
        std::error_code ec;
        std::filesystem::remove(batch.file_name, ec);
    }
    batch_files_.clear();
    if (!all_input_file_.empty()) {
        std::error_code ec;
        std::filesystem::remove(all_input_file_, ec);
        all_input_file_.clear();
    }
}

std::string AggregateExecutor::BuildGroupKey(const Tuple &tuple, std::vector<Value> *group_vals) const {
    group_vals->clear();
    std::ostringstream key_builder;
    for (const auto &expr : group_by_exprs_) {
        Value gv = expr->Evaluate(&tuple, &child_->GetOutputSchema(), nullptr, nullptr);
        group_vals->push_back(gv);
        key_builder << SerializeValueForHash(gv) << "|";
    }
    return key_builder.str();
}

bool AggregateExecutor::LoadNextBatch() {
    results_.clear();
    pos_ = 0;

    const std::vector<TypeId> agg_value_types =
        AggregateValueTypesFromSchema(output_schema_, group_by_exprs_.size(), agg_types_.size());

    while (current_batch_idx_ < batch_files_.size()) {
        const auto &batch = batch_files_[current_batch_idx_++];
        std::ifstream in(batch.file_name, std::ios::binary);
        if (!in) throw std::runtime_error("Failed to open aggregate batch file");

        struct GroupState {
            std::vector<Value> group_vals;
            std::vector<AggTransition> aggs;
        };

        std::unordered_map<std::string, GroupState> groups;
        Tuple input;
        std::vector<Value> group_vals;
        while (ReadTupleFromStream(&in, child_->GetOutputSchema(), &input)) {
            std::string group_key = BuildGroupKey(input, &group_vals);
            auto &state = groups[group_key];
            if (state.aggs.empty()) {
                state.group_vals = group_vals;
                state.aggs = MakeInitialTransitions(agg_types_, agg_value_types);
            }
            ApplyAggregateRow(input, child_->GetOutputSchema(), agg_types_, agg_exprs_, &state.aggs);
        }
        in.close();

        if (!groups.empty()) {
            results_.reserve(groups.size());
            for (auto &entry : groups) {
                results_.push_back(BuildAggregateOutputTuple(entry.second.group_vals, entry.second.aggs, agg_value_types));
            }
            return true;
        }
    }
    return false;
}

void AggregateExecutor::Init() {
    child_->Init();
    RemoveBatchFiles();
    results_.clear();
    pos_ = 0;
    current_batch_idx_ = 0;
    emit_empty_ = false;

    std::size_t bytes_written = 0;
    all_input_file_ = MaterializeChildInput(&bytes_written);
    child_->Close();

    if (bytes_written == 0) {
        if (group_by_exprs_.empty()) emit_empty_ = true;
        return;
    }

    if (bytes_written <= memory_budget_bytes_) {
        batch_files_.push_back(AggregateBatchInfo{all_input_file_, bytes_written});
        return;
    }

    std::size_t nbatches = NextPowerOfTwo(std::max<std::size_t>(2, (bytes_written + memory_budget_bytes_ - 1) / memory_budget_bytes_));
    batch_files_.assign(nbatches, AggregateBatchInfo{});
    std::vector<std::ofstream> outs(nbatches);
    for (std::size_t i = 0; i < nbatches; i++) {
        batch_files_[i].file_name = MakeTempRunFileName();
        outs[i].open(batch_files_[i].file_name, std::ios::binary);
        if (!outs[i]) throw std::runtime_error("Failed to create aggregate batch file");
    }

    std::ifstream in(all_input_file_, std::ios::binary);
    Tuple input;
    std::vector<Value> group_vals;
    while (ReadTupleFromStream(&in, child_->GetOutputSchema(), &input)) {
        std::string key = BuildGroupKey(input, &group_vals);
        std::size_t batch_no = std::hash<std::string>{}(key) & (nbatches - 1);
        WriteTupleToStream(&outs[batch_no], input, child_->GetOutputSchema());
        batch_files_[batch_no].bytes += EstimateTupleSize(input);
    }
    in.close();
    for (auto &out : outs) out.close();
}

bool AggregateExecutor::Next(Tuple *out_tuple) {
    if (emit_empty_) {
        emit_empty_ = false;
        const auto agg_value_types = AggregateValueTypesFromSchema(output_schema_, group_by_exprs_.size(), agg_types_.size());
        auto empty_aggs = MakeInitialTransitions(agg_types_, agg_value_types);
        *out_tuple = BuildAggregateOutputTuple({}, empty_aggs, agg_value_types);
        return true;
    }

    while (true) {
        if (pos_ < results_.size()) {
            *out_tuple = results_[pos_++];
            return true;
        }
        if (!LoadNextBatch()) return false;
    }
}

void AggregateExecutor::Close() {
    child_->Close();
    results_.clear();
    pos_ = 0;
    current_batch_idx_ = 0;
    emit_empty_ = false;
    RemoveBatchFiles();
}

const Schema &AggregateExecutor::GetOutputSchema() const { return output_schema_; }

StreamAggregateExecutor::StreamAggregateExecutor(std::unique_ptr<AbstractExecutor> child,
                                                 std::vector<std::unique_ptr<AbstractExpression>> group_by_exprs,
                                                 std::vector<AggregateType> agg_types,
                                                 std::vector<std::unique_ptr<AbstractExpression>> agg_exprs,
                                                 Schema output_schema)
    : child_(std::move(child)), group_by_exprs_(std::move(group_by_exprs)), agg_types_(std::move(agg_types)),
      agg_exprs_(std::move(agg_exprs)), output_schema_(std::move(output_schema)), have_lookahead_(false),
      has_current_group_(false), emit_empty_(false), finished_(false), pos_(0) {
    if (agg_types_.size() != agg_exprs_.size()) {
        throw std::runtime_error("Aggregate types and aggregate expressions size mismatch");
    }
}

void StreamAggregateExecutor::StartGroup(const Tuple &input) {
    current_group_vals_.clear();
    current_group_eval_vals_.clear();
    const auto agg_value_types = AggregateValueTypesFromSchema(output_schema_, group_by_exprs_.size(), agg_types_.size());
    current_agg_output_vals_.assign(agg_types_.size(), Value::Null(TypeId::INT64));
    current_agg_initialized_.assign(agg_types_.size(), false);

    for (const auto &expr : group_by_exprs_) {
        Value gv = expr->Evaluate(&input, &child_->GetOutputSchema(), nullptr, nullptr);
        current_group_vals_.push_back(gv);
        current_group_eval_vals_.push_back(gv);
    }
    ApplyAggregateRowStreaming(input, child_->GetOutputSchema(), agg_types_, agg_exprs_,
                               &current_agg_output_vals_, &current_agg_initialized_, agg_value_types);
    has_current_group_ = true;
}

bool StreamAggregateExecutor::SameGroup(const Tuple &input) const {
    if (!has_current_group_) return false;
    for (std::size_t i = 0; i < group_by_exprs_.size(); i++) {
        Value gv = group_by_exprs_[i]->Evaluate(&input, &child_->GetOutputSchema(), nullptr, nullptr);
        if (CompareValues(gv, current_group_eval_vals_[i]) != 0) return false;
    }
    return true;
}

void StreamAggregateExecutor::Init() {
    child_->Init();
    have_lookahead_ = child_->Next(&lookahead_tuple_);
    has_current_group_ = false;
    emit_empty_ = !have_lookahead_ && group_by_exprs_.empty();
    finished_ = false;
    current_group_vals_.clear();
    current_group_eval_vals_.clear();
    current_agg_output_vals_.clear();
    current_agg_initialized_.clear();
    results_.clear();
    pos_ = 0;
}

bool StreamAggregateExecutor::Next(Tuple *out_tuple) {
    const auto agg_value_types = AggregateValueTypesFromSchema(output_schema_, group_by_exprs_.size(), agg_types_.size());

    if (emit_empty_) {
        emit_empty_ = false;
        std::vector<Value> empty_vals;
        std::vector<bool> empty_init(agg_types_.size(), false);
        *out_tuple = BuildStreamingAggregateOutputTuple({}, empty_vals, empty_init, agg_types_, agg_value_types);
        return true;
    }

    if (finished_) return false;
    if (!has_current_group_) {
        if (!have_lookahead_) return false;
        StartGroup(lookahead_tuple_);
        have_lookahead_ = false;
    }

    Tuple input;
    while (true) {
        if (!child_->Next(&input)) {
            finished_ = true;
            *out_tuple = BuildStreamingAggregateOutputTuple(
                current_group_vals_, current_agg_output_vals_, current_agg_initialized_, agg_types_, agg_value_types);
            has_current_group_ = false;
            return true;
        }
        if (SameGroup(input)) {
            ApplyAggregateRowStreaming(input, child_->GetOutputSchema(), agg_types_, agg_exprs_,
                                       &current_agg_output_vals_, &current_agg_initialized_, agg_value_types);
            continue;
        }

        lookahead_tuple_ = input;
        have_lookahead_ = true;
        Tuple result = BuildStreamingAggregateOutputTuple(
            current_group_vals_, current_agg_output_vals_, current_agg_initialized_, agg_types_, agg_value_types);
        StartGroup(lookahead_tuple_);
        have_lookahead_ = false;
        *out_tuple = result;
        return true;
    }
}

void StreamAggregateExecutor::Close() {
    child_->Close();
    have_lookahead_ = false;
    has_current_group_ = false;
    emit_empty_ = false;
    finished_ = false;
    current_group_vals_.clear();
    current_group_eval_vals_.clear();
    current_agg_output_vals_.clear();
    current_agg_initialized_.clear();
    results_.clear();
    pos_ = 0;
}

const Schema &StreamAggregateExecutor::GetOutputSchema() const { return output_schema_; }

}  // namespace simpledb
