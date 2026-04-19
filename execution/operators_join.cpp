// Join executors: nested-loop, index nested-loop, hash join, and merge join.

#include "operators_common.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <queue>
#include <stdexcept>
#include <unordered_map>

namespace simpledb {
using namespace execution_detail;

NestedLoopJoinExecutor::NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left_child,
                                               std::unique_ptr<AbstractExecutor> right_child,
                                               std::unique_ptr<AbstractExpression> join_predicate)
    : left_child_(std::move(left_child)), right_child_(std::move(right_child)),
      join_predicate_(std::move(join_predicate)),
      output_schema_(ConcatSchemas(left_child_->GetOutputSchema(), right_child_->GetOutputSchema())),
      left_loaded_(false) {}

void NestedLoopJoinExecutor::Init() {
    left_child_->Init();
    right_child_->Init();
    left_loaded_ = false;
}

bool NestedLoopJoinExecutor::Next(Tuple *out_tuple) {
    Tuple right_tuple;
    while (true) {
        if (!left_loaded_) {
            if (!left_child_->Next(&current_left_tuple_)) return false;
            left_loaded_ = true;
            right_child_->Close();
            right_child_->Init();
        }
        while (right_child_->Next(&right_tuple)) {
            Value pred = join_predicate_->Evaluate(
                &current_left_tuple_, &left_child_->GetOutputSchema(), &right_tuple, &right_child_->GetOutputSchema());
            if (ValueAsBool(pred)) {
                *out_tuple = ConcatTuples(current_left_tuple_, right_tuple);
                return true;
            }
        }
        left_loaded_ = false;
    }
}

void NestedLoopJoinExecutor::Close() {
    left_child_->Close();
    right_child_->Close();
}

const Schema &NestedLoopJoinExecutor::GetOutputSchema() const { return output_schema_; }

IndexNestedLoopJoinExecutor::IndexNestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left_child,
                                                         const HeapFile *right_heap_file,
                                                         const BTreeIndex *right_index,
                                                         std::unique_ptr<AbstractExpression> left_key_expr,
                                                         TransactionPtr txn,
                                                         LockManager *lock_manager)
    : left_child_(std::move(left_child)), right_heap_file_(right_heap_file), right_index_(right_index),
      txn_(std::move(txn)), lock_manager_(lock_manager), left_key_expr_(std::move(left_key_expr)),
      output_schema_(ConcatSchemas(left_child_->GetOutputSchema(), right_heap_file_->GetSchema())),
      left_loaded_(false), current_match_pos_(0) {}

void IndexNestedLoopJoinExecutor::Init() {
    left_child_->Init();
    left_loaded_ = false;
    current_matches_.clear();
    current_match_pos_ = 0;
}

bool IndexNestedLoopJoinExecutor::Next(Tuple *out_tuple) {
    while (true) {
        if (!left_loaded_) {
            if (!left_child_->Next(&current_left_tuple_)) return false;
            left_loaded_ = true;
            Value key = left_key_expr_->Evaluate(&current_left_tuple_, &left_child_->GetOutputSchema(), nullptr, nullptr);
            current_matches_ = right_index_->Search(key);
            current_match_pos_ = 0;
        }
        while (current_match_pos_ < current_matches_.size()) {
            RID rid = current_matches_[current_match_pos_++];
            Tuple right_tuple;
            bool ok = txn_ != nullptr ? right_heap_file_->GetTuple(txn_, rid, &right_tuple) : right_heap_file_->GetTuple(rid, &right_tuple);
            if (ok) {
                *out_tuple = ConcatTuples(current_left_tuple_, right_tuple);
                return true;
            }
        }
        left_loaded_ = false;
    }
}

void IndexNestedLoopJoinExecutor::Close() {
    left_child_->Close();
    current_matches_.clear();
    current_match_pos_ = 0;
}

const Schema &IndexNestedLoopJoinExecutor::GetOutputSchema() const { return output_schema_; }

MemoizedIndexNestedLoopJoinExecutor::MemoizedIndexNestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left_child,
                                                                             const HeapFile *right_heap_file,
                                                                             const BTreeIndex *right_index,
                                                                             std::unique_ptr<AbstractExpression> left_key_expr,
                                                                             TransactionPtr txn,
                                                                             LockManager *lock_manager)
    : left_child_(std::move(left_child)), right_heap_file_(right_heap_file), right_index_(right_index),
      txn_(std::move(txn)), lock_manager_(lock_manager), left_key_expr_(std::move(left_key_expr)),
      output_schema_(ConcatSchemas(left_child_->GetOutputSchema(), right_heap_file_->GetSchema())),
      left_loaded_(false), current_match_pos_(0) {}

void MemoizedIndexNestedLoopJoinExecutor::Init() {
    left_child_->Init();
    left_loaded_ = false;
    current_matches_.clear();
    current_match_pos_ = 0;
    memo_cache_.clear();
}

bool MemoizedIndexNestedLoopJoinExecutor::Next(Tuple *out_tuple) {
    while (true) {
        if (!left_loaded_) {
            if (!left_child_->Next(&current_left_tuple_)) return false;
            left_loaded_ = true;
            Value key = left_key_expr_->Evaluate(&current_left_tuple_, &left_child_->GetOutputSchema(), nullptr, nullptr);
            std::string encoded_key = SerializeValueForHash(key);
            auto it = memo_cache_.find(encoded_key);
            if (it == memo_cache_.end()) {
                CachedMatchSet cached;
                std::vector<RID> rids = right_index_->Search(key);
                for (const RID &rid : rids) {
                    Tuple right_tuple;
                    bool ok = txn_ != nullptr ? right_heap_file_->GetTuple(txn_, rid, &right_tuple)
                                              : right_heap_file_->GetTuple(rid, &right_tuple);
                    if (ok) cached.tuples.push_back(std::move(right_tuple));
                }
                it = memo_cache_.emplace(encoded_key, std::move(cached)).first;
            }
            current_matches_ = it->second.tuples;
            current_match_pos_ = 0;
        }
        while (current_match_pos_ < current_matches_.size()) {
            *out_tuple = ConcatTuples(current_left_tuple_, current_matches_[current_match_pos_++]);
            return true;
        }
        left_loaded_ = false;
    }
}

void MemoizedIndexNestedLoopJoinExecutor::Close() {
    left_child_->Close();
    current_matches_.clear();
    current_match_pos_ = 0;
    memo_cache_.clear();
}

const Schema &MemoizedIndexNestedLoopJoinExecutor::GetOutputSchema() const { return output_schema_; }

HashJoinExecutor::HashJoinExecutor(std::unique_ptr<AbstractExecutor> left_child,
                                   std::unique_ptr<AbstractExecutor> right_child,
                                   std::unique_ptr<AbstractExpression> left_key_expr,
                                   std::unique_ptr<AbstractExpression> right_key_expr,
                                   bool build_left_side,
                                   std::size_t memory_budget_bytes)
    : left_child_(std::move(left_child)), right_child_(std::move(right_child)),
      left_key_expr_(std::move(left_key_expr)), right_key_expr_(std::move(right_key_expr)),
      build_left_side_(build_left_side), memory_budget_bytes_(memory_budget_bytes),
      output_schema_(ConcatSchemas(left_child_->GetOutputSchema(), right_child_->GetOutputSchema())),
      nbatches_(1), current_batch_idx_(0), current_match_pos_(0) {}

std::string HashJoinExecutor::MaterializeExecutorToFile(AbstractExecutor *exec,
                                                        const Schema &schema,
                                                        std::size_t *bytes_written) {
    std::string file_name = MakeTempRunFileName();
    std::ofstream out(file_name, std::ios::binary);
    if (!out) throw std::runtime_error("Failed to create hash join temp file");
    *bytes_written = 0;
    Tuple tuple;
    while (exec->Next(&tuple)) {
        WriteTupleToStream(&out, tuple, schema);
        *bytes_written += EstimateTupleSize(tuple);
    }
    out.close();
    return file_name;
}

void HashJoinExecutor::RemoveTempFile(const std::string &file_name) {
    if (file_name.empty()) return;
    std::error_code ec;
    std::filesystem::remove(file_name, ec);
}

void HashJoinExecutor::RemoveBatchFiles() {
    for (const auto &batch : batch_files_) {
        RemoveTempFile(batch.build_file);
        RemoveTempFile(batch.probe_file);
    }
    batch_files_.clear();
}

std::string HashJoinExecutor::EvaluateBuildKey(const Tuple &tuple) const {
    const Schema &schema = build_left_side_ ? left_child_->GetOutputSchema() : right_child_->GetOutputSchema();
    const AbstractExpression *expr = build_left_side_ ? left_key_expr_.get() : right_key_expr_.get();
    return SerializeValueForHash(expr->Evaluate(&tuple, &schema, nullptr, nullptr));
}

std::string HashJoinExecutor::EvaluateProbeKey(const Tuple &tuple) const {
    const Schema &schema = build_left_side_ ? right_child_->GetOutputSchema() : left_child_->GetOutputSchema();
    const AbstractExpression *expr = build_left_side_ ? right_key_expr_.get() : left_key_expr_.get();
    return SerializeValueForHash(expr->Evaluate(&tuple, &schema, nullptr, nullptr));
}

std::size_t HashJoinExecutor::HashKey(const std::string &key) const {
    return std::hash<std::string>{}(key);
}

void HashJoinExecutor::PartitionInputsIntoBatches() {
    RemoveBatchFiles();
    nbatches_ = 1;

    const Schema &build_schema = build_left_side_ ? left_child_->GetOutputSchema() : right_child_->GetOutputSchema();
    const Schema &probe_schema = build_left_side_ ? right_child_->GetOutputSchema() : left_child_->GetOutputSchema();

    while (true) {
        batch_files_.assign(nbatches_, BatchFileInfo{});
        std::vector<std::ofstream> build_outs(nbatches_);
        std::vector<std::ofstream> probe_outs(nbatches_);
        for (std::size_t i = 0; i < nbatches_; i++) {
            batch_files_[i].build_file = MakeTempRunFileName();
            batch_files_[i].probe_file = MakeTempRunFileName();
            build_outs[i].open(batch_files_[i].build_file, std::ios::binary);
            probe_outs[i].open(batch_files_[i].probe_file, std::ios::binary);
            if (!build_outs[i] || !probe_outs[i]) throw std::runtime_error("Failed to create hash join batch files");
        }

        {
            std::ifstream in(build_all_file_, std::ios::binary);
            Tuple tuple;
            while (ReadTupleFromStream(&in, build_schema, &tuple)) {
                std::string key = EvaluateBuildKey(tuple);
                std::size_t batch_no = HashKey(key) & (nbatches_ - 1);
                WriteTupleToStream(&build_outs[batch_no], tuple, build_schema);
                batch_files_[batch_no].build_bytes += EstimateTupleSize(tuple);
            }
        }

        {
            std::ifstream in(probe_all_file_, std::ios::binary);
            Tuple tuple;
            while (ReadTupleFromStream(&in, probe_schema, &tuple)) {
                std::string key = EvaluateProbeKey(tuple);
                std::size_t batch_no = HashKey(key) & (nbatches_ - 1);
                WriteTupleToStream(&probe_outs[batch_no], tuple, probe_schema);
                batch_files_[batch_no].probe_bytes += EstimateTupleSize(tuple);
            }
        }

        for (auto &out : build_outs) out.close();
        for (auto &out : probe_outs) out.close();

        std::size_t max_build_bytes = 0;
        for (const auto &batch : batch_files_) max_build_bytes = std::max(max_build_bytes, batch.build_bytes);
        if (max_build_bytes <= memory_budget_bytes_ || nbatches_ >= 1024) break;

        RemoveBatchFiles();
        nbatches_ <<= 1;
    }
}

bool HashJoinExecutor::LoadNextBatch() {
    const Schema &build_schema = build_left_side_ ? left_child_->GetOutputSchema() : right_child_->GetOutputSchema();
    const Schema &probe_schema = build_left_side_ ? right_child_->GetOutputSchema() : left_child_->GetOutputSchema();

    hash_table_.clear();
    current_matches_.clear();
    current_match_pos_ = 0;
    if (current_probe_input_.is_open()) current_probe_input_.close();

    while (current_batch_idx_ < batch_files_.size()) {
        const auto &batch = batch_files_[current_batch_idx_++];

        std::ifstream build_in(batch.build_file, std::ios::binary);
        Tuple tuple;
        while (ReadTupleFromStream(&build_in, build_schema, &tuple)) {
            std::string key = EvaluateBuildKey(tuple);
            hash_table_[key].push_back(BuildTuplePayload{key, tuple});
        }
        build_in.close();

        current_probe_input_.open(batch.probe_file, std::ios::binary);
        if (!current_probe_input_) throw std::runtime_error("Failed to open probe batch file");
        return true;
    }
    return false;
}

Tuple HashJoinExecutor::BuildJoinedTuple(const Tuple &build_tuple, const Tuple &probe_tuple) const {
    if (build_left_side_) return ConcatTuples(build_tuple, probe_tuple);
    return ConcatTuples(probe_tuple, build_tuple);
}

void HashJoinExecutor::Init() {
    left_child_->Init();
    right_child_->Init();
    RemoveTempFile(build_all_file_);
    RemoveTempFile(probe_all_file_);
    RemoveBatchFiles();

    std::size_t build_bytes = 0;
    std::size_t probe_bytes = 0;
    build_all_file_ = MaterializeExecutorToFile(build_left_side_ ? left_child_.get() : right_child_.get(),
                                                build_left_side_ ? left_child_->GetOutputSchema() : right_child_->GetOutputSchema(),
                                                &build_bytes);
    probe_all_file_ = MaterializeExecutorToFile(build_left_side_ ? right_child_.get() : left_child_.get(),
                                                build_left_side_ ? right_child_->GetOutputSchema() : left_child_->GetOutputSchema(),
                                                &probe_bytes);
    left_child_->Close();
    right_child_->Close();

    PartitionInputsIntoBatches();
    current_batch_idx_ = 0;
    current_match_pos_ = 0;
    current_matches_.clear();
    hash_table_.clear();
    LoadNextBatch();
}

bool HashJoinExecutor::Next(Tuple *out_tuple) {
    const Schema &probe_schema = build_left_side_ ? right_child_->GetOutputSchema() : left_child_->GetOutputSchema();

    while (true) {
        if (current_match_pos_ < current_matches_.size()) {
            *out_tuple = BuildJoinedTuple(current_matches_[current_match_pos_++].tuple, current_probe_tuple_);
            return true;
        }

        Tuple probe_tuple;
        while (current_probe_input_.is_open() && ReadTupleFromStream(&current_probe_input_, probe_schema, &probe_tuple)) {
            current_probe_tuple_ = probe_tuple;
            std::string key = EvaluateProbeKey(current_probe_tuple_);
            auto it = hash_table_.find(key);
            current_matches_.clear();
            if (it != hash_table_.end()) {
                current_matches_ = it->second;
                current_match_pos_ = 0;
                break;
            }
        }

        if (current_match_pos_ < current_matches_.size()) continue;
        if (!LoadNextBatch()) return false;
    }
}

void HashJoinExecutor::Close() {
    left_child_->Close();
    right_child_->Close();
    hash_table_.clear();
    current_matches_.clear();
    current_match_pos_ = 0;
    if (current_probe_input_.is_open()) current_probe_input_.close();
    RemoveBatchFiles();
    RemoveTempFile(build_all_file_);
    RemoveTempFile(probe_all_file_);
    build_all_file_.clear();
    probe_all_file_.clear();
}

const Schema &HashJoinExecutor::GetOutputSchema() const { return output_schema_; }

MergeJoinExecutor::MergeJoinExecutor(std::unique_ptr<AbstractExecutor> left_child,
                                     std::unique_ptr<AbstractExecutor> right_child,
                                     std::unique_ptr<AbstractExpression> left_key_expr,
                                     std::unique_ptr<AbstractExpression> right_key_expr)
    : left_child_(std::move(left_child)), right_child_(std::move(right_child)),
      left_key_expr_(std::move(left_key_expr)), right_key_expr_(std::move(right_key_expr)),
      output_schema_(ConcatSchemas(left_child_->GetOutputSchema(), right_child_->GetOutputSchema())),
      has_left_tuple_(false), has_right_tuple_(false), inner_marked_(false), has_pending_outer_tuple_(false),
      state_(State::NEED_COMPARE) {}

Value MergeJoinExecutor::EvaluateLeftKey(const Tuple &tuple) const {
    return left_key_expr_->Evaluate(&tuple, &left_child_->GetOutputSchema(), nullptr, nullptr);
}

Value MergeJoinExecutor::EvaluateRightKey(const Tuple &tuple) const {
    return right_key_expr_->Evaluate(&tuple, &right_child_->GetOutputSchema(), nullptr, nullptr);
}

void MergeJoinExecutor::ClearMatchState() {
    inner_marked_ = false;
    has_pending_outer_tuple_ = false;
}

void MergeJoinExecutor::Init() {
    left_child_->Init();
    right_child_->Init();
    if (!right_child_->SupportsMarkRestore()) {
        throw std::runtime_error("MergeJoinExecutor requires mark/restore support on inner child");
    }
    has_left_tuple_ = left_child_->Next(&current_left_tuple_);
    has_right_tuple_ = right_child_->Next(&current_right_tuple_);
    ClearMatchState();
    state_ = State::NEED_COMPARE;
}

bool MergeJoinExecutor::Next(Tuple *out_tuple) {
    while (true) {
        switch (state_) {
            case State::EXHAUSTED:
                return false;

            case State::NEED_COMPARE: {
                if (!has_left_tuple_ || !has_right_tuple_) {
                    state_ = State::EXHAUSTED;
                    continue;
                }
                int cmp = CompareValues(EvaluateLeftKey(current_left_tuple_), EvaluateRightKey(current_right_tuple_));
                if (cmp < 0) {
                    has_left_tuple_ = left_child_->Next(&current_left_tuple_);
                } else if (cmp > 0) {
                    has_right_tuple_ = right_child_->Next(&current_right_tuple_);
                } else {
                    current_match_key_ = EvaluateLeftKey(current_left_tuple_);
                    marked_left_tuple_ = current_left_tuple_;
                    marked_right_tuple_ = current_right_tuple_;
                    right_child_->MarkPosition();
                    inner_marked_ = true;
                    state_ = State::EMIT_MATCHES;
                }
                break;
            }

            case State::EMIT_MATCHES: {
                if (!has_left_tuple_ || !has_right_tuple_) {
                    state_ = State::EXHAUSTED;
                    continue;
                }
                *out_tuple = ConcatTuples(current_left_tuple_, current_right_tuple_);
                has_right_tuple_ = right_child_->Next(&current_right_tuple_);
                if (!has_right_tuple_ || CompareValues(EvaluateRightKey(current_right_tuple_), current_match_key_) != 0) {
                    state_ = State::ADVANCE_OUTER_DUPLICATES;
                }
                return true;
            }

            case State::ADVANCE_OUTER_DUPLICATES: {
                has_left_tuple_ = left_child_->Next(&current_left_tuple_);
                if (has_left_tuple_ && CompareValues(EvaluateLeftKey(current_left_tuple_), current_match_key_) == 0) {
                    current_right_tuple_ = marked_right_tuple_;
                    has_right_tuple_ = true;
                    right_child_->RestorePosition();
                    state_ = State::EMIT_MATCHES;
                } else {
                    ClearMatchState();
                    state_ = State::NEED_COMPARE;
                }
                break;
            }
        }
    }
}

void MergeJoinExecutor::Close() {
    left_child_->Close();
    right_child_->Close();
    ClearMatchState();
    state_ = State::EXHAUSTED;
}

const Schema &MergeJoinExecutor::GetOutputSchema() const { return output_schema_; }

}  // namespace simpledb
