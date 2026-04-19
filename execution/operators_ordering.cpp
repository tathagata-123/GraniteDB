// Ordering, top-N, limit, materialization, and RID materialization executors.

#include "operators_common.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <queue>
#include <stdexcept>
#include <unordered_set>

namespace simpledb {
using namespace execution_detail;

SortExecutor::SortExecutor(std::unique_ptr<AbstractExecutor> child,
                           std::vector<SortKeySpec> sort_keys,
                           std::size_t run_capacity,
                           std::size_t merge_fan_in)
    : child_(std::move(child)), sort_keys_(std::move(sort_keys)), run_capacity_(run_capacity), merge_fan_in_(merge_fan_in),
      using_in_memory_output_(false), pos_(0), mark_pos_(0), current_stream_pos_(0), mark_stream_pos_(0) {}

int SortExecutor::CompareTuples(const Tuple &a, const Tuple &b) const {
    return CompareTupleBySortKeys(a, b, sort_keys_, child_->GetOutputSchema());
}

std::string SortExecutor::WriteRunFile(std::vector<Tuple> run) {
    std::sort(run.begin(), run.end(), [&](const Tuple &a, const Tuple &b) { return CompareTuples(a, b) < 0; });
    std::string file_name = MakeTempRunFileName();
    std::ofstream out(file_name, std::ios::binary);
    if (!out) throw std::runtime_error("Failed to create external sort run file");
    for (const auto &tuple : run) WriteTupleToStream(&out, tuple, child_->GetOutputSchema());
    out.close();
    run_files_.push_back(file_name);
    return file_name;
}

bool SortExecutor::ReadNextTupleFromRun(std::ifstream *in, Tuple *tuple) const {
    return ReadTupleFromStream(in, child_->GetOutputSchema(), tuple);
}

std::string SortExecutor::MergeRunGroup(const std::vector<std::string> &group) {
    struct HeapEntry {
        Tuple tuple;
        std::size_t run_idx{0};
    };
    auto cmp = [&](const HeapEntry &a, const HeapEntry &b) { return CompareTuples(a.tuple, b.tuple) > 0; };
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype(cmp)> pq(cmp);

    std::vector<std::ifstream> run_inputs(group.size());
    for (std::size_t i = 0; i < group.size(); i++) {
        run_inputs[i].open(group[i], std::ios::binary);
        if (!run_inputs[i]) throw std::runtime_error("Failed to open external sort run file");
        Tuple first;
        if (ReadNextTupleFromRun(&run_inputs[i], &first)) pq.push(HeapEntry{first, i});
    }

    std::string merged = MakeTempRunFileName();
    std::ofstream out(merged, std::ios::binary);
    if (!out) throw std::runtime_error("Failed to create merged run file");

    while (!pq.empty()) {
        HeapEntry top = pq.top();
        pq.pop();
        WriteTupleToStream(&out, top.tuple, child_->GetOutputSchema());
        Tuple next_tuple;
        if (ReadNextTupleFromRun(&run_inputs[top.run_idx], &next_tuple)) {
            pq.push(HeapEntry{next_tuple, top.run_idx});
        }
    }

    for (auto &in : run_inputs) in.close();
    out.close();
    run_files_.push_back(merged);
    return merged;
}

void SortExecutor::RemoveRunFiles() {
    std::unordered_set<std::string> files;
    for (const auto &file : run_files_) files.insert(file);
    if (!final_run_file_.empty()) files.insert(final_run_file_);
    for (const auto &file : files) {
        std::error_code ec;
        std::filesystem::remove(file, ec);
    }
    run_files_.clear();
    final_run_file_.clear();
}

void SortExecutor::Init() {
    child_->Init();
    RemoveRunFiles();
    in_memory_output_.clear();
    using_in_memory_output_ = false;
    pos_ = 0;
    mark_pos_ = 0;
    final_run_input_.reset();
    current_stream_pos_ = 0;
    mark_stream_pos_ = 0;

    std::vector<Tuple> current_run;
    current_run.reserve(run_capacity_);
    Tuple tuple;
    while (child_->Next(&tuple)) {
        current_run.push_back(tuple);
        if (current_run.size() >= run_capacity_) {
            WriteRunFile(std::move(current_run));
            current_run.clear();
            current_run.reserve(run_capacity_);
        }
    }

    if (run_files_.empty()) {
        using_in_memory_output_ = true;
        in_memory_output_ = std::move(current_run);
        std::sort(in_memory_output_.begin(), in_memory_output_.end(),
                  [&](const Tuple &a, const Tuple &b) { return CompareTuples(a, b) < 0; });
        return;
    }

    if (!current_run.empty()) WriteRunFile(std::move(current_run));

    std::vector<std::string> current_files = run_files_;
    while (current_files.size() > 1) {
        std::vector<std::string> next_files;
        for (std::size_t i = 0; i < current_files.size(); i += merge_fan_in_) {
            std::size_t end = std::min(current_files.size(), i + merge_fan_in_);
            std::vector<std::string> group(current_files.begin() + static_cast<std::ptrdiff_t>(i),
                                           current_files.begin() + static_cast<std::ptrdiff_t>(end));
            if (group.size() == 1) next_files.push_back(group[0]);
            else next_files.push_back(MergeRunGroup(group));
        }
        current_files = std::move(next_files);
    }

    final_run_file_ = current_files.front();
    final_run_input_ = std::make_unique<std::ifstream>(final_run_file_, std::ios::binary);
    if (!(*final_run_input_)) throw std::runtime_error("Failed to open final sort run file");
    current_stream_pos_ = final_run_input_->tellg();
    mark_stream_pos_ = current_stream_pos_;
}

bool SortExecutor::Next(Tuple *out_tuple) {
    if (using_in_memory_output_) {
        if (pos_ >= in_memory_output_.size()) return false;
        *out_tuple = in_memory_output_[pos_++];
        return true;
    }
    if (!final_run_input_) return false;
    if (!ReadNextTupleFromRun(final_run_input_.get(), out_tuple)) return false;
    current_stream_pos_ = final_run_input_->tellg();
    return true;
}

void SortExecutor::Close() {
    child_->Close();
    in_memory_output_.clear();
    pos_ = 0;
    mark_pos_ = 0;
    final_run_input_.reset();
    RemoveRunFiles();
}

const Schema &SortExecutor::GetOutputSchema() const { return child_->GetOutputSchema(); }
bool SortExecutor::SupportsMarkRestore() const { return true; }

void SortExecutor::MarkPosition() {
    if (using_in_memory_output_) mark_pos_ = pos_;
    else mark_stream_pos_ = current_stream_pos_;
}

void SortExecutor::RestorePosition() {
    if (using_in_memory_output_) {
        pos_ = mark_pos_;
        return;
    }
    if (!final_run_input_) return;
    final_run_input_->clear();
    final_run_input_->seekg(mark_stream_pos_);
    current_stream_pos_ = mark_stream_pos_;
}

TopNExecutor::TopNExecutor(std::unique_ptr<AbstractExecutor> child,
                           std::vector<SortKeySpec> sort_keys,
                           std::size_t limit_count)
    : child_(std::move(child)), sort_keys_(std::move(sort_keys)), limit_count_(limit_count), pos_(0) {}

int TopNExecutor::CompareTuples(const Tuple &a, const Tuple &b) const {
    return CompareTupleBySortKeys(a, b, sort_keys_, child_->GetOutputSchema());
}

void TopNExecutor::Init() {
    child_->Init();
    output_.clear();
    pos_ = 0;
    if (limit_count_ == 0) return;

    std::vector<Tuple> heap;
    auto better = [&](const Tuple &a, const Tuple &b) { return CompareTuples(a, b) < 0; };
    auto worse_on_top = [&](const Tuple &a, const Tuple &b) { return better(a, b); };

    Tuple tuple;
    while (child_->Next(&tuple)) {
        if (heap.size() < limit_count_) {
            heap.push_back(tuple);
            std::push_heap(heap.begin(), heap.end(), worse_on_top);
        } else if (CompareTuples(tuple, heap.front()) < 0) {
            std::pop_heap(heap.begin(), heap.end(), worse_on_top);
            heap.back() = tuple;
            std::push_heap(heap.begin(), heap.end(), worse_on_top);
        }
    }

    output_ = std::move(heap);
    std::sort(output_.begin(), output_.end(), [&](const Tuple &a, const Tuple &b) { return CompareTuples(a, b) < 0; });
}

bool TopNExecutor::Next(Tuple *out_tuple) {
    if (pos_ >= output_.size()) return false;
    *out_tuple = output_[pos_++];
    return true;
}

void TopNExecutor::Close() {
    child_->Close();
    output_.clear();
    pos_ = 0;
}

const Schema &TopNExecutor::GetOutputSchema() const { return child_->GetOutputSchema(); }

LimitExecutor::LimitExecutor(std::unique_ptr<AbstractExecutor> child,
                             std::size_t limit_count)
    : child_(std::move(child)), limit_count_(limit_count), produced_count_(0) {}

void LimitExecutor::Init() {
    child_->Init();
    produced_count_ = 0;
}

bool LimitExecutor::Next(Tuple *out_tuple) {
    if (produced_count_ >= limit_count_) return false;
    if (!child_->Next(out_tuple)) return false;
    produced_count_++;
    return true;
}

void LimitExecutor::Close() {
    child_->Close();
    produced_count_ = 0;
}

const Schema &LimitExecutor::GetOutputSchema() const { return child_->GetOutputSchema(); }

MaterializeExecutor::MaterializeExecutor(std::unique_ptr<AbstractExecutor> child)
    : child_(std::move(child)), pos_(0), mark_pos_(0) {}

void MaterializeExecutor::Init() {
    child_->Init();
    tuples_.clear();
    pos_ = 0;
    mark_pos_ = 0;
    Tuple tuple;
    while (child_->Next(&tuple)) tuples_.push_back(tuple);
    child_->Close();
}

bool MaterializeExecutor::Next(Tuple *out_tuple) {
    if (pos_ >= tuples_.size()) return false;
    *out_tuple = tuples_[pos_++];
    return true;
}

void MaterializeExecutor::Close() {
    tuples_.clear();
    pos_ = 0;
    mark_pos_ = 0;
    child_->Close();
}

const Schema &MaterializeExecutor::GetOutputSchema() const { return child_->GetOutputSchema(); }
bool MaterializeExecutor::SupportsMarkRestore() const { return true; }
void MaterializeExecutor::MarkPosition() { mark_pos_ = pos_; }
void MaterializeExecutor::RestorePosition() { pos_ = mark_pos_; }

RidListExecutor::RidListExecutor(std::vector<RID> rids)
    : rids_(std::move(rids)),
      output_schema_(Schema({Column("rid_page", TypeId::INT32, false), Column("rid_slot", TypeId::INT32, false)})),
      pos_(0) {}

void RidListExecutor::Init() { pos_ = 0; }

bool RidListExecutor::Next(Tuple *out_tuple) {
    if (pos_ >= rids_.size()) return false;
    const RID &rid = rids_[pos_++];
    *out_tuple = Tuple({Value(static_cast<int32_t>(rid.page_no)), Value(static_cast<int32_t>(rid.slot_no))});
    return true;
}

void RidListExecutor::Close() { pos_ = 0; }
const Schema &RidListExecutor::GetOutputSchema() const { return output_schema_; }

MaterializeRidsExecutor::MaterializeRidsExecutor(std::unique_ptr<AbstractExecutor> child,
                                                 const HeapFile *heap_file,
                                                 TransactionPtr txn)
    : child_(std::move(child)), heap_file_(heap_file), txn_(std::move(txn)) {}

void MaterializeRidsExecutor::Init() { child_->Init(); }

bool MaterializeRidsExecutor::Next(Tuple *out_tuple) {
    Tuple rid_tuple;
    while (child_->Next(&rid_tuple)) {
        RID rid;
        rid.page_no = static_cast<PageNo>(rid_tuple.GetValue(0).AsInt32());
        rid.slot_no = static_cast<SlotNo>(rid_tuple.GetValue(1).AsInt32());
        bool ok = txn_ != nullptr ? heap_file_->GetTuple(txn_, rid, out_tuple) : heap_file_->GetTuple(rid, out_tuple);
        if (ok) return true;
    }
    return false;
}

void MaterializeRidsExecutor::Close() { child_->Close(); }
const Schema &MaterializeRidsExecutor::GetOutputSchema() const { return heap_file_->GetSchema(); }

}  // namespace simpledb
