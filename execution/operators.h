// Executor interfaces and concrete operator declarations.

#pragma once

#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../access/btree.h"
#include "../access/btree_iterator.h"
#include "../access/generic_btree.h"
#include "../access/heap_file.h"
#include "../access/heap_file_iterator.h"
#include "../common/schema.h"
#include "../common/tuple.h"
#include "../concurrency/lock_manager.h"
#include "../concurrency/transaction.h"
#include "executor.h"
#include "expressions.h"

namespace simpledb {

Schema ConcatSchemas(const Schema &left, const Schema &right);
Tuple ConcatTuples(const Tuple &left, const Tuple &right);

class SeqScanExecutor : public AbstractExecutor {
public:
    explicit SeqScanExecutor(const HeapFile *heap_file,
                             TransactionPtr txn = nullptr,
                             LockManager *lock_manager = nullptr);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    const HeapFile *heap_file_;
    TransactionPtr txn_;
    LockManager *lock_manager_;
    std::unique_ptr<HeapFileIterator> iter_;
};

class IndexScanExecutor : public AbstractExecutor {
public:
    IndexScanExecutor(const HeapFile *heap_file, const BTreeIndex *index,
                      TransactionPtr txn = nullptr,
                      LockManager *lock_manager = nullptr);
    IndexScanExecutor(const HeapFile *heap_file, const BTreeIndex *index, Value equality_key,
                      TransactionPtr txn = nullptr,
                      LockManager *lock_manager = nullptr);
    IndexScanExecutor(const HeapFile *heap_file,
                      const BTreeIndex *index,
                      std::optional<Value> lower_bound,
                      std::optional<Value> upper_bound,
                      bool lower_inclusive = true,
                      bool upper_inclusive = true,
                      TransactionPtr txn = nullptr,
                      LockManager *lock_manager = nullptr);

    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    enum class ScanMode { FULL, EQUALITY, RANGE };
    bool KeyWithinBounds(const Value &key) const;

    const HeapFile *heap_file_;
    const BTreeIndex *index_;
    TransactionPtr txn_;
    LockManager *lock_manager_;
    Schema output_schema_;
    ScanMode mode_;
    std::optional<Value> equality_key_;
    std::optional<Value> lower_bound_;
    std::optional<Value> upper_bound_;
    bool lower_inclusive_;
    bool upper_inclusive_;
    std::unique_ptr<BTreeIndexIterator> iter_;
};

class GenericIndexScanExecutor : public AbstractExecutor {
public:
    GenericIndexScanExecutor(const HeapFile *heap_file,
                             const GenericBTreeIndex *index,
                             GenericBTreeIndex::PrefixScanSpec scan_spec,
                             bool full_index_scan = false,
                             TransactionPtr txn = nullptr,
                             LockManager *lock_manager = nullptr);

    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    const HeapFile *heap_file_;
    const GenericBTreeIndex *index_;
    GenericBTreeIndex::PrefixScanSpec scan_spec_;
    bool full_index_scan_;
    TransactionPtr txn_;
    LockManager *lock_manager_;
    Schema output_schema_;
    std::vector<RID> hits_;
    std::size_t pos_;
};

class BitmapIndexScanExecutor : public AbstractExecutor {
public:
    BitmapIndexScanExecutor(const BTreeIndex *index,
                            std::optional<Value> equality_key,
                            std::optional<Value> lower_bound,
                            std::optional<Value> upper_bound,
                            bool lower_inclusive = true,
                            bool upper_inclusive = true);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    bool KeyWithinBounds(const Value &key) const;

    const BTreeIndex *index_;
    Schema output_schema_;
    std::optional<Value> equality_key_;
    std::optional<Value> lower_bound_;
    std::optional<Value> upper_bound_;
    bool lower_inclusive_;
    bool upper_inclusive_;
    std::vector<RID> hits_;
    std::size_t pos_;
};

class BitmapAndExecutor : public AbstractExecutor {
public:
    explicit BitmapAndExecutor(std::vector<std::unique_ptr<AbstractExecutor>> children);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    std::vector<std::unique_ptr<AbstractExecutor>> children_;
    Schema output_schema_;
    std::vector<RID> hits_;
    std::size_t pos_;
};

class BitmapOrExecutor : public AbstractExecutor {
public:
    explicit BitmapOrExecutor(std::vector<std::unique_ptr<AbstractExecutor>> children);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    std::vector<std::unique_ptr<AbstractExecutor>> children_;
    Schema output_schema_;
    std::vector<RID> hits_;
    std::size_t pos_;
};

class BitmapHeapScanExecutor : public AbstractExecutor {
public:
    BitmapHeapScanExecutor(std::unique_ptr<AbstractExecutor> bitmap_child,
                           const HeapFile *heap_file,
                           TransactionPtr txn = nullptr,
                           LockManager *lock_manager = nullptr);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    std::unique_ptr<AbstractExecutor> bitmap_child_;
    const HeapFile *heap_file_;
    TransactionPtr txn_;
    LockManager *lock_manager_;
};

class IndexOnlyScanExecutor : public AbstractExecutor {
public:
    IndexOnlyScanExecutor(const Schema &heap_schema,
                          const BTreeIndex *index,
                          std::optional<Value> equality_key,
                          std::optional<Value> lower_bound,
                          std::optional<Value> upper_bound,
                          bool lower_inclusive,
                          bool upper_inclusive,
                          std::unordered_map<std::size_t, std::size_t> column_to_key_pos);
    IndexOnlyScanExecutor(const Schema &heap_schema,
                          const GenericBTreeIndex *index,
                          GenericBTreeIndex::PrefixScanSpec scan_spec,
                          bool full_index_scan,
                          std::unordered_map<std::size_t, std::size_t> column_to_key_pos);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    Tuple BuildOutputTuple(const std::vector<Value> &key_values) const;
    bool NextSingleColumnEntry(std::vector<Value> *key_values);
    bool NextGenericEntry(std::vector<Value> *key_values);

    Schema output_schema_;
    const BTreeIndex *btree_index_;
    const GenericBTreeIndex *generic_index_;
    std::optional<Value> equality_key_;
    std::optional<Value> lower_bound_;
    std::optional<Value> upper_bound_;
    bool lower_inclusive_;
    bool upper_inclusive_;
    GenericBTreeIndex::PrefixScanSpec scan_spec_;
    bool full_index_scan_;
    std::unordered_map<std::size_t, std::size_t> column_to_key_pos_;
    std::vector<RID> equality_hits_;
    std::size_t equality_pos_;
    std::unique_ptr<BTreeIndexIterator> btree_iter_;
    std::vector<GenericBTreeIndex::KeyRidEntry> generic_entries_;
    std::size_t generic_pos_;
};

class FilterExecutor : public AbstractExecutor {
public:
    FilterExecutor(std::unique_ptr<AbstractExecutor> child,
                   std::unique_ptr<AbstractExpression> predicate);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    std::unique_ptr<AbstractExecutor> child_;
    std::unique_ptr<AbstractExpression> predicate_;
};

class ProjectExecutor : public AbstractExecutor {
public:
    ProjectExecutor(std::unique_ptr<AbstractExecutor> child,
                    std::vector<std::unique_ptr<AbstractExpression>> projections,
                    Schema output_schema);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    std::unique_ptr<AbstractExecutor> child_;
    std::vector<std::unique_ptr<AbstractExpression>> projections_;
    Schema output_schema_;
};


class AppendExecutor : public AbstractExecutor {
public:
    AppendExecutor(std::vector<std::unique_ptr<AbstractExecutor>> children,
                   Schema output_schema);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    std::vector<std::unique_ptr<AbstractExecutor>> children_;
    Schema output_schema_;
    std::size_t child_idx_;
};

class UniqueExecutor : public AbstractExecutor {
public:
    explicit UniqueExecutor(std::unique_ptr<AbstractExecutor> child);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    std::unique_ptr<AbstractExecutor> child_;
    bool has_prev_;
    Tuple prev_tuple_;
};

enum class SetOpMode {
    INTERSECT,
    EXCEPT
};

class SetOpExecutor : public AbstractExecutor {
public:
    SetOpExecutor(std::unique_ptr<AbstractExecutor> left_child,
                  std::unique_ptr<AbstractExecutor> right_child,
                  SetOpMode mode,
                  bool all,
                  Schema output_schema);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    bool ReadNextGroup(AbstractExecutor *child,
                       Tuple *group_value,
                       std::size_t *group_count,
                       bool *has_pending,
                       Tuple *pending_tuple);
    void AdvanceLeftGroup();
    void AdvanceRightGroup();

    std::unique_ptr<AbstractExecutor> left_child_;
    std::unique_ptr<AbstractExecutor> right_child_;
    SetOpMode mode_;
    bool all_;
    Schema output_schema_;

    bool left_has_pending_;
    bool right_has_pending_;
    Tuple left_pending_tuple_;
    Tuple right_pending_tuple_;
    bool has_left_group_;
    bool has_right_group_;
    Tuple current_left_group_;
    Tuple current_right_group_;
    std::size_t current_left_count_;
    std::size_t current_right_count_;
    bool emit_ready_;
    Tuple emit_tuple_;
    std::size_t emit_remaining_;
};

class NestedLoopJoinExecutor : public AbstractExecutor {
public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left_child,
                           std::unique_ptr<AbstractExecutor> right_child,
                           std::unique_ptr<AbstractExpression> join_predicate);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    std::unique_ptr<AbstractExecutor> left_child_;
    std::unique_ptr<AbstractExecutor> right_child_;
    std::unique_ptr<AbstractExpression> join_predicate_;
    Schema output_schema_;
    bool left_loaded_;
    Tuple current_left_tuple_;
};

class IndexNestedLoopJoinExecutor : public AbstractExecutor {
public:
    IndexNestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left_child,
                                const HeapFile *right_heap_file,
                                const BTreeIndex *right_index,
                                std::unique_ptr<AbstractExpression> left_key_expr,
                                TransactionPtr txn = nullptr,
                                LockManager *lock_manager = nullptr);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    std::unique_ptr<AbstractExecutor> left_child_;
    const HeapFile *right_heap_file_;
    const BTreeIndex *right_index_;
    TransactionPtr txn_;
    LockManager *lock_manager_;
    std::unique_ptr<AbstractExpression> left_key_expr_;
    Schema output_schema_;
    bool left_loaded_;
    Tuple current_left_tuple_;
    std::vector<RID> current_matches_;
    std::size_t current_match_pos_;
};

class MemoizedIndexNestedLoopJoinExecutor : public AbstractExecutor {
public:
    MemoizedIndexNestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left_child,
                                        const HeapFile *right_heap_file,
                                        const BTreeIndex *right_index,
                                        std::unique_ptr<AbstractExpression> left_key_expr,
                                        TransactionPtr txn = nullptr,
                                        LockManager *lock_manager = nullptr);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    struct CachedMatchSet {
        std::vector<Tuple> tuples;
    };

    std::unique_ptr<AbstractExecutor> left_child_;
    const HeapFile *right_heap_file_;
    const BTreeIndex *right_index_;
    TransactionPtr txn_;
    LockManager *lock_manager_;
    std::unique_ptr<AbstractExpression> left_key_expr_;
    Schema output_schema_;
    bool left_loaded_;
    Tuple current_left_tuple_;
    std::vector<Tuple> current_matches_;
    std::size_t current_match_pos_;
    std::unordered_map<std::string, CachedMatchSet> memo_cache_;
};

class HashJoinExecutor : public AbstractExecutor {
public:
    HashJoinExecutor(std::unique_ptr<AbstractExecutor> left_child,
                     std::unique_ptr<AbstractExecutor> right_child,
                     std::unique_ptr<AbstractExpression> left_key_expr,
                     std::unique_ptr<AbstractExpression> right_key_expr,
                     bool build_left_side,
                     std::size_t memory_budget_bytes = 1 << 20);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    struct BuildTuplePayload {
        std::string key;
        Tuple tuple;
    };

    struct BatchFileInfo {
        std::string build_file;
        std::string probe_file;
        std::size_t build_bytes{0};
        std::size_t probe_bytes{0};
    };

    std::string MaterializeExecutorToFile(AbstractExecutor *exec, const Schema &schema, std::size_t *bytes_written);
    void RemoveTempFile(const std::string &file_name);
    void RemoveBatchFiles();
    std::string EvaluateBuildKey(const Tuple &tuple) const;
    std::string EvaluateProbeKey(const Tuple &tuple) const;
    std::size_t HashKey(const std::string &key) const;
    void PartitionInputsIntoBatches();
    bool LoadNextBatch();
    Tuple BuildJoinedTuple(const Tuple &build_tuple, const Tuple &probe_tuple) const;

    std::unique_ptr<AbstractExecutor> left_child_;
    std::unique_ptr<AbstractExecutor> right_child_;
    std::unique_ptr<AbstractExpression> left_key_expr_;
    std::unique_ptr<AbstractExpression> right_key_expr_;
    bool build_left_side_;
    std::size_t memory_budget_bytes_;
    Schema output_schema_;

    std::size_t nbatches_;
    std::vector<BatchFileInfo> batch_files_;
    std::string build_all_file_;
    std::string probe_all_file_;

    std::unordered_map<std::string, std::vector<BuildTuplePayload>> hash_table_;
    std::size_t current_batch_idx_;
    std::ifstream current_probe_input_;
    Tuple current_probe_tuple_;
    std::vector<BuildTuplePayload> current_matches_;
    std::size_t current_match_pos_;
};

class MergeJoinExecutor : public AbstractExecutor {
public:
    MergeJoinExecutor(std::unique_ptr<AbstractExecutor> left_child,
                      std::unique_ptr<AbstractExecutor> right_child,
                      std::unique_ptr<AbstractExpression> left_key_expr,
                      std::unique_ptr<AbstractExpression> right_key_expr);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    enum class State {
        NEED_COMPARE,
        EMIT_MATCHES,
        ADVANCE_OUTER_DUPLICATES,
        EXHAUSTED
    };

    Value EvaluateLeftKey(const Tuple &tuple) const;
    Value EvaluateRightKey(const Tuple &tuple) const;
    void ClearMatchState();

    std::unique_ptr<AbstractExecutor> left_child_;
    std::unique_ptr<AbstractExecutor> right_child_;
    std::unique_ptr<AbstractExpression> left_key_expr_;
    std::unique_ptr<AbstractExpression> right_key_expr_;
    Schema output_schema_;

    bool has_left_tuple_;
    bool has_right_tuple_;
    Tuple current_left_tuple_;
    Tuple current_right_tuple_;

    bool inner_marked_;
    Value current_match_key_ = Value::Null(TypeId::INT32);
    Tuple marked_left_tuple_;
    Tuple marked_right_tuple_;
    Tuple pending_outer_tuple_;
    bool has_pending_outer_tuple_;
    State state_;
};

struct SortKeySpec {
    std::unique_ptr<AbstractExpression> expr;
    bool ascending{true};
};

class SortExecutor : public AbstractExecutor {
public:
    SortExecutor(std::unique_ptr<AbstractExecutor> child,
                 std::vector<SortKeySpec> sort_keys,
                 std::size_t run_capacity = 256,
                 std::size_t merge_fan_in = 8);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;
    bool SupportsMarkRestore() const override;
    void MarkPosition() override;
    void RestorePosition() override;

private:
    int CompareTuples(const Tuple &a, const Tuple &b) const;
    std::string WriteRunFile(std::vector<Tuple> run);
    bool ReadNextTupleFromRun(std::ifstream *in, Tuple *tuple) const;
    std::string MergeRunGroup(const std::vector<std::string> &group);
    void RemoveRunFiles();

    std::unique_ptr<AbstractExecutor> child_;
    std::vector<SortKeySpec> sort_keys_;
    std::size_t run_capacity_;
    std::size_t merge_fan_in_;
    std::vector<Tuple> in_memory_output_;
    bool using_in_memory_output_;
    std::size_t pos_;
    std::size_t mark_pos_;
    std::vector<std::string> run_files_;
    std::string final_run_file_;
    std::unique_ptr<std::ifstream> final_run_input_;
    std::streampos current_stream_pos_;
    std::streampos mark_stream_pos_;
};

class TopNExecutor : public AbstractExecutor {
public:
    TopNExecutor(std::unique_ptr<AbstractExecutor> child,
                 std::vector<SortKeySpec> sort_keys,
                 std::size_t limit_count);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    int CompareTuples(const Tuple &a, const Tuple &b) const;

    std::unique_ptr<AbstractExecutor> child_;
    std::vector<SortKeySpec> sort_keys_;
    std::size_t limit_count_;
    std::vector<Tuple> output_;
    std::size_t pos_;
};

class LimitExecutor : public AbstractExecutor {
public:
    LimitExecutor(std::unique_ptr<AbstractExecutor> child,
                  std::size_t limit_count);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    std::unique_ptr<AbstractExecutor> child_;
    std::size_t limit_count_;
    std::size_t produced_count_;
};

class MaterializeExecutor : public AbstractExecutor {
public:
    explicit MaterializeExecutor(std::unique_ptr<AbstractExecutor> child);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;
    bool SupportsMarkRestore() const override;
    void MarkPosition() override;
    void RestorePosition() override;

private:
    std::unique_ptr<AbstractExecutor> child_;
    std::vector<Tuple> tuples_;
    std::size_t pos_;
    std::size_t mark_pos_;
};

class RidListExecutor : public AbstractExecutor {
public:
    explicit RidListExecutor(std::vector<RID> rids);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    std::vector<RID> rids_;
    Schema output_schema_;
    std::size_t pos_;
};

class MaterializeRidsExecutor : public AbstractExecutor {
public:
    MaterializeRidsExecutor(std::unique_ptr<AbstractExecutor> child,
                            const HeapFile *heap_file,
                            TransactionPtr txn = nullptr);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    std::unique_ptr<AbstractExecutor> child_;
    const HeapFile *heap_file_;
    TransactionPtr txn_;
};

enum class AggregateType {
    COUNT,
    SUM,
    MIN,
    MAX
};

class AggregateExecutor : public AbstractExecutor {
public:
    AggregateExecutor(std::unique_ptr<AbstractExecutor> child,
                      std::vector<std::unique_ptr<AbstractExpression>> group_by_exprs,
                      std::vector<AggregateType> agg_types,
                      std::vector<std::unique_ptr<AbstractExpression>> agg_exprs,
                      Schema output_schema,
                      std::size_t memory_budget_bytes = 1 << 20);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    struct AggregateBatchInfo {
        std::string file_name;
        std::size_t bytes{0};
    };

    std::string MaterializeChildInput(std::size_t *bytes_written);
    void RemoveBatchFiles();
    std::string BuildGroupKey(const Tuple &tuple, std::vector<Value> *group_vals) const;
    bool LoadNextBatch();

    std::unique_ptr<AbstractExecutor> child_;
    std::vector<std::unique_ptr<AbstractExpression>> group_by_exprs_;
    std::vector<AggregateType> agg_types_;
    std::vector<std::unique_ptr<AbstractExpression>> agg_exprs_;
    Schema output_schema_;
    std::size_t memory_budget_bytes_;
    std::vector<Tuple> results_;
    std::size_t pos_;
    std::string all_input_file_;
    std::vector<AggregateBatchInfo> batch_files_;
    std::size_t current_batch_idx_;
    bool emit_empty_;
};

class StreamAggregateExecutor : public AbstractExecutor {
public:
    StreamAggregateExecutor(std::unique_ptr<AbstractExecutor> child,
                            std::vector<std::unique_ptr<AbstractExpression>> group_by_exprs,
                            std::vector<AggregateType> agg_types,
                            std::vector<std::unique_ptr<AbstractExpression>> agg_exprs,
                            Schema output_schema);
    void Init() override;
    bool Next(Tuple *out_tuple) override;
    void Close() override;
    const Schema &GetOutputSchema() const override;

private:
    void StartGroup(const Tuple &input);
    bool SameGroup(const Tuple &input) const;

    std::unique_ptr<AbstractExecutor> child_;
    std::vector<std::unique_ptr<AbstractExpression>> group_by_exprs_;
    std::vector<AggregateType> agg_types_;
    std::vector<std::unique_ptr<AbstractExpression>> agg_exprs_;
    Schema output_schema_;

    bool have_lookahead_;
    Tuple lookahead_tuple_;
    bool has_current_group_;
    bool emit_empty_;
    bool finished_;
    std::vector<Value> current_group_vals_;
    std::vector<Value> current_group_eval_vals_;
    std::vector<Value> current_agg_output_vals_;
    std::vector<bool> current_agg_initialized_;
    std::size_t pos_;
    std::vector<Tuple> results_;
};

}  // namespace simpledb
