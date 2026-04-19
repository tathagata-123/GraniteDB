// Scan, bitmap, and index-only executors.

#include "operators_common.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace simpledb {
using namespace execution_detail;

SeqScanExecutor::SeqScanExecutor(const HeapFile *heap_file,
                                 TransactionPtr txn,
                                 LockManager *lock_manager)
    : heap_file_(heap_file), txn_(std::move(txn)), lock_manager_(lock_manager) {}

void SeqScanExecutor::Init() {
    if (txn_ != nullptr && lock_manager_ != nullptr) {
        if (!lock_manager_->LockTable(txn_, heap_file_->GetRelationId(), LockMode::S)) {
            throw std::runtime_error("Failed to acquire phantom-safe table S lock for seq scan");
        }
    }
    if (txn_ != nullptr) iter_ = std::make_unique<HeapFileIterator>(heap_file_, txn_, lock_manager_);
    else iter_ = std::make_unique<HeapFileIterator>(heap_file_);
}

bool SeqScanExecutor::Next(Tuple *out_tuple) {
    if (!iter_ || !iter_->HasNext()) return false;
    auto [rid, tuple] = iter_->Next();
    (void)rid;
    *out_tuple = tuple;
    return true;
}

void SeqScanExecutor::Close() { iter_.reset(); }
const Schema &SeqScanExecutor::GetOutputSchema() const { return heap_file_->GetSchema(); }

IndexScanExecutor::IndexScanExecutor(const HeapFile *heap_file, const BTreeIndex *index,
                                     TransactionPtr txn,
                                     LockManager *lock_manager)
    : heap_file_(heap_file), index_(index), txn_(std::move(txn)), lock_manager_(lock_manager),
      output_schema_(heap_file->GetSchema()), mode_(ScanMode::FULL),
      lower_inclusive_(true), upper_inclusive_(true) {}

IndexScanExecutor::IndexScanExecutor(const HeapFile *heap_file, const BTreeIndex *index, Value equality_key,
                                     TransactionPtr txn,
                                     LockManager *lock_manager)
    : heap_file_(heap_file), index_(index), txn_(std::move(txn)), lock_manager_(lock_manager),
      output_schema_(heap_file->GetSchema()), mode_(ScanMode::EQUALITY), equality_key_(std::move(equality_key)),
      lower_inclusive_(true), upper_inclusive_(true) {}

IndexScanExecutor::IndexScanExecutor(const HeapFile *heap_file,
                                     const BTreeIndex *index,
                                     std::optional<Value> lower_bound,
                                     std::optional<Value> upper_bound,
                                     bool lower_inclusive,
                                     bool upper_inclusive,
                                     TransactionPtr txn,
                                     LockManager *lock_manager)
    : heap_file_(heap_file), index_(index), txn_(std::move(txn)), lock_manager_(lock_manager),
      output_schema_(heap_file->GetSchema()), mode_(ScanMode::RANGE),
      lower_bound_(std::move(lower_bound)), upper_bound_(std::move(upper_bound)),
      lower_inclusive_(lower_inclusive), upper_inclusive_(upper_inclusive) {}

void IndexScanExecutor::Init() {
    if (txn_ != nullptr && lock_manager_ != nullptr) {
        bool ok = false;
        if (mode_ == ScanMode::EQUALITY) {
            ok = lock_manager_->LockKey(txn_, index_->GetIndexRelationId(), { *equality_key_ }, LockMode::S);
        } else {
            std::optional<std::vector<Value>> lower;
            std::optional<std::vector<Value>> upper;
            if (lower_bound_.has_value()) lower = std::vector<Value>{*lower_bound_};
            if (upper_bound_.has_value()) upper = std::vector<Value>{*upper_bound_};
            ok = lock_manager_->LockKeyRange(txn_, index_->GetIndexRelationId(), lower, upper,
                                             lower_inclusive_, upper_inclusive_, LockMode::S);
        }
        if (!ok) {
            throw std::runtime_error("Failed to acquire phantom-safe key-range lock for index scan");
        }
    }

    if (mode_ == ScanMode::EQUALITY) {
        iter_ = std::make_unique<BTreeIndexIterator>(index_, *equality_key_);
    } else if (mode_ == ScanMode::RANGE && lower_bound_.has_value()) {
        iter_ = std::make_unique<BTreeIndexIterator>(index_, *lower_bound_);
    } else {
        iter_ = std::make_unique<BTreeIndexIterator>(index_);
    }
}

bool IndexScanExecutor::KeyWithinBounds(const Value &key) const {
    if (mode_ == ScanMode::EQUALITY) {
        return CompareValues(key, *equality_key_) == 0;
    }
    if (lower_bound_.has_value()) {
        int cmp = CompareValues(key, *lower_bound_);
        if (cmp < 0 || (cmp == 0 && !lower_inclusive_)) return false;
    }
    if (upper_bound_.has_value()) {
        int cmp = CompareValues(key, *upper_bound_);
        if (cmp > 0 || (cmp == 0 && !upper_inclusive_)) return false;
    }
    return true;
}

bool IndexScanExecutor::Next(Tuple *out_tuple) {
    while (iter_ && iter_->HasNext()) {
        auto [key, rid] = iter_->Next();
        if (mode_ == ScanMode::EQUALITY) {
            int cmp = CompareValues(key, *equality_key_);
            if (cmp != 0) return false;
        } else {
            if (lower_bound_.has_value()) {
                int cmp = CompareValues(key, *lower_bound_);
                if (cmp < 0 || (cmp == 0 && !lower_inclusive_)) continue;
            }
            if (upper_bound_.has_value()) {
                int cmp = CompareValues(key, *upper_bound_);
                if (cmp > 0 || (cmp == 0 && !upper_inclusive_)) return false;
            }
        }

        Tuple tuple;
        bool ok = txn_ != nullptr ? heap_file_->GetTuple(txn_, rid, &tuple) : heap_file_->GetTuple(rid, &tuple);
        if (ok) {
            *out_tuple = tuple;
            return true;
        }
    }
    return false;
}

void IndexScanExecutor::Close() { iter_.reset(); }
const Schema &IndexScanExecutor::GetOutputSchema() const { return output_schema_; }

GenericIndexScanExecutor::GenericIndexScanExecutor(const HeapFile *heap_file,
                                                   const GenericBTreeIndex *index,
                                                   GenericBTreeIndex::PrefixScanSpec scan_spec,
                                                   bool full_index_scan,
                                                   TransactionPtr txn,
                                                   LockManager *lock_manager)
    : heap_file_(heap_file), index_(index), scan_spec_(std::move(scan_spec)), full_index_scan_(full_index_scan),
      txn_(std::move(txn)), lock_manager_(lock_manager), output_schema_(heap_file->GetSchema()), pos_(0) {}

void GenericIndexScanExecutor::Init() {
    if (txn_ != nullptr && lock_manager_ != nullptr) {
        if (!lock_manager_->LockTable(txn_, heap_file_->GetRelationId(), LockMode::S)) {
            throw std::runtime_error("Failed to acquire coarse phantom-safe table S lock for generic index scan");
        }
    }
    hits_.clear();
    pos_ = 0;
    if (full_index_scan_) hits_ = index_->FullScanRids();
    else hits_ = index_->ScanPrefixRange(scan_spec_);
}

bool GenericIndexScanExecutor::Next(Tuple *out_tuple) {
    while (pos_ < hits_.size()) {
        RID rid = hits_[pos_++];
        Tuple tuple;
        bool ok = txn_ != nullptr ? heap_file_->GetTuple(txn_, rid, &tuple) : heap_file_->GetTuple(rid, &tuple);
        if (ok) {
            *out_tuple = tuple;
            return true;
        }
    }
    return false;
}

void GenericIndexScanExecutor::Close() {
    hits_.clear();
    pos_ = 0;
}

const Schema &GenericIndexScanExecutor::GetOutputSchema() const { return output_schema_; }

BitmapIndexScanExecutor::BitmapIndexScanExecutor(const BTreeIndex *index,
                                                 std::optional<Value> equality_key,
                                                 std::optional<Value> lower_bound,
                                                 std::optional<Value> upper_bound,
                                                 bool lower_inclusive,
                                                 bool upper_inclusive)
    : index_(index),
      output_schema_(Schema({Column("rid_page", TypeId::INT32, false), Column("rid_slot", TypeId::INT32, false)})),
      equality_key_(std::move(equality_key)),
      lower_bound_(std::move(lower_bound)),
      upper_bound_(std::move(upper_bound)),
      lower_inclusive_(lower_inclusive),
      upper_inclusive_(upper_inclusive),
      pos_(0) {}

bool BitmapIndexScanExecutor::KeyWithinBounds(const Value &key) const {
    if (equality_key_.has_value()) return CompareValues(key, *equality_key_) == 0;
    if (lower_bound_.has_value()) {
        int cmp = CompareValues(key, *lower_bound_);
        if (cmp < 0 || (cmp == 0 && !lower_inclusive_)) return false;
    }
    if (upper_bound_.has_value()) {
        int cmp = CompareValues(key, *upper_bound_);
        if (cmp > 0 || (cmp == 0 && !upper_inclusive_)) return false;
    }
    return true;
}

void BitmapIndexScanExecutor::Init() {
    hits_.clear();
    pos_ = 0;
    if (equality_key_.has_value()) {
        hits_ = index_->Search(*equality_key_);
        return;
    }
    std::unique_ptr<BTreeIndexIterator> iter;
    if (lower_bound_.has_value()) iter = std::make_unique<BTreeIndexIterator>(index_, *lower_bound_);
    else iter = std::make_unique<BTreeIndexIterator>(index_);
    while (iter->HasNext()) {
        auto [key, rid] = iter->Next();
        if (!KeyWithinBounds(key)) {
            if (upper_bound_.has_value()) {
                int cmp = CompareValues(key, *upper_bound_);
                if (cmp > 0 || (cmp == 0 && !upper_inclusive_)) break;
            }
            continue;
        }
        hits_.push_back(rid);
    }
}

bool BitmapIndexScanExecutor::Next(Tuple *out_tuple) {
    if (pos_ >= hits_.size()) return false;
    *out_tuple = MakeRidTuple(hits_[pos_++]);
    return true;
}

void BitmapIndexScanExecutor::Close() {
    hits_.clear();
    pos_ = 0;
}

const Schema &BitmapIndexScanExecutor::GetOutputSchema() const { return output_schema_; }

BitmapAndExecutor::BitmapAndExecutor(std::vector<std::unique_ptr<AbstractExecutor>> children)
    : children_(std::move(children)),
      output_schema_(Schema({Column("rid_page", TypeId::INT32, false), Column("rid_slot", TypeId::INT32, false)})),
      pos_(0) {}

void BitmapAndExecutor::Init() {
    hits_.clear();
    pos_ = 0;
    if (children_.empty()) return;

    std::unordered_set<uint64_t> current;
    children_[0]->Init();
    Tuple tuple;
    while (children_[0]->Next(&tuple)) current.insert(EncodeRid(DecodeRidTuple(tuple)));
    children_[0]->Close();

    for (std::size_t i = 1; i < children_.size() && !current.empty(); i++) {
        std::unordered_set<uint64_t> next_set;
        children_[i]->Init();
        while (children_[i]->Next(&tuple)) {
            uint64_t enc = EncodeRid(DecodeRidTuple(tuple));
            if (current.count(enc) != 0) next_set.insert(enc);
        }
        children_[i]->Close();
        current.swap(next_set);
    }

    hits_.reserve(current.size());
    for (uint64_t enc : current) {
        hits_.push_back(RID{static_cast<PageNo>(enc >> 32), static_cast<SlotNo>(enc & 0xFFFF)});
    }
    std::sort(hits_.begin(), hits_.end(), CompareRids);
}

bool BitmapAndExecutor::Next(Tuple *out_tuple) {
    if (pos_ >= hits_.size()) return false;
    *out_tuple = MakeRidTuple(hits_[pos_++]);
    return true;
}

void BitmapAndExecutor::Close() {
    hits_.clear();
    pos_ = 0;
}

const Schema &BitmapAndExecutor::GetOutputSchema() const { return output_schema_; }

BitmapOrExecutor::BitmapOrExecutor(std::vector<std::unique_ptr<AbstractExecutor>> children)
    : children_(std::move(children)), output_schema_(Schema({Column("page_no", TypeId::INT32), Column("slot_no", TypeId::INT32)})),
      pos_(0) {}

void BitmapOrExecutor::Init() {
    hits_.clear();
    pos_ = 0;
    std::unordered_set<uint64_t> seen;
    for (auto &child : children_) {
        child->Init();
        Tuple tuple;
        while (child->Next(&tuple)) {
            RID rid = DecodeRidTuple(tuple);
            uint64_t encoded = EncodeRid(rid);
            if (seen.insert(encoded).second) hits_.push_back(rid);
        }
        child->Close();
    }
    std::sort(hits_.begin(), hits_.end(), CompareRids);
}

bool BitmapOrExecutor::Next(Tuple *out_tuple) {
    if (pos_ >= hits_.size()) return false;
    *out_tuple = MakeRidTuple(hits_[pos_++]);
    return true;
}

void BitmapOrExecutor::Close() {
    hits_.clear();
    pos_ = 0;
}

const Schema &BitmapOrExecutor::GetOutputSchema() const { return output_schema_; }

BitmapHeapScanExecutor::BitmapHeapScanExecutor(std::unique_ptr<AbstractExecutor> bitmap_child,
                                               const HeapFile *heap_file,
                                               TransactionPtr txn,
                                               LockManager *lock_manager)
    : bitmap_child_(std::move(bitmap_child)), heap_file_(heap_file), txn_(std::move(txn)), lock_manager_(lock_manager) {}

void BitmapHeapScanExecutor::Init() { bitmap_child_->Init(); }

bool BitmapHeapScanExecutor::Next(Tuple *out_tuple) {
    Tuple rid_tuple;
    while (bitmap_child_->Next(&rid_tuple)) {
        RID rid = DecodeRidTuple(rid_tuple);
        Tuple tuple;
        bool ok = txn_ != nullptr ? heap_file_->GetTuple(txn_, rid, &tuple) : heap_file_->GetTuple(rid, &tuple);
        if (ok) {
            *out_tuple = tuple;
            return true;
        }
    }
    return false;
}

void BitmapHeapScanExecutor::Close() { bitmap_child_->Close(); }
const Schema &BitmapHeapScanExecutor::GetOutputSchema() const { return heap_file_->GetSchema(); }

IndexOnlyScanExecutor::IndexOnlyScanExecutor(const Schema &heap_schema,
                                             const BTreeIndex *index,
                                             std::optional<Value> equality_key,
                                             std::optional<Value> lower_bound,
                                             std::optional<Value> upper_bound,
                                             bool lower_inclusive,
                                             bool upper_inclusive,
                                             std::unordered_map<std::size_t, std::size_t> column_to_key_pos)
    : output_schema_(heap_schema),
      btree_index_(index),
      generic_index_(nullptr),
      equality_key_(std::move(equality_key)),
      lower_bound_(std::move(lower_bound)),
      upper_bound_(std::move(upper_bound)),
      lower_inclusive_(lower_inclusive),
      upper_inclusive_(upper_inclusive),
      full_index_scan_(false),
      column_to_key_pos_(std::move(column_to_key_pos)),
      equality_pos_(0),
      generic_pos_(0) {}

IndexOnlyScanExecutor::IndexOnlyScanExecutor(const Schema &heap_schema,
                                             const GenericBTreeIndex *index,
                                             GenericBTreeIndex::PrefixScanSpec scan_spec,
                                             bool full_index_scan,
                                             std::unordered_map<std::size_t, std::size_t> column_to_key_pos)
    : output_schema_(heap_schema),
      btree_index_(nullptr),
      generic_index_(index),
      lower_inclusive_(true),
      upper_inclusive_(true),
      scan_spec_(std::move(scan_spec)),
      full_index_scan_(full_index_scan),
      column_to_key_pos_(std::move(column_to_key_pos)),
      equality_pos_(0),
      generic_pos_(0) {}

void IndexOnlyScanExecutor::Init() {
    equality_hits_.clear();
    equality_pos_ = 0;
    btree_iter_.reset();
    generic_entries_.clear();
    generic_pos_ = 0;

    if (btree_index_ != nullptr) {
        if (equality_key_.has_value()) {
            equality_hits_ = btree_index_->Search(*equality_key_);
        } else if (lower_bound_.has_value()) {
            btree_iter_ = std::make_unique<BTreeIndexIterator>(btree_index_, *lower_bound_);
        } else {
            btree_iter_ = std::make_unique<BTreeIndexIterator>(btree_index_);
        }
        return;
    }

    if (full_index_scan_) generic_entries_ = generic_index_->FullScanEntries();
    else if (scan_spec_.equality_prefix.size() == generic_index_->GetDefinition().key_columns.size()) {
        generic_entries_ = generic_index_->SearchExactEntries(scan_spec_.equality_prefix);
    } else {
        generic_entries_ = generic_index_->ScanPrefixRangeEntries(scan_spec_);
    }
}

Tuple IndexOnlyScanExecutor::BuildOutputTuple(const std::vector<Value> &key_values) const {
    std::vector<Value> values;
    values.reserve(output_schema_.GetColumnCount());
    for (std::size_t i = 0; i < output_schema_.GetColumnCount(); i++) {
        auto it = column_to_key_pos_.find(i);
        if (it != column_to_key_pos_.end() && it->second < key_values.size()) values.push_back(key_values[it->second]);
        else values.push_back(Value::Null(output_schema_.GetColumn(i).GetType()));
    }
    return Tuple(std::move(values));
}

bool IndexOnlyScanExecutor::NextSingleColumnEntry(std::vector<Value> *key_values) {
    if (equality_key_.has_value()) {
        if (equality_pos_ >= equality_hits_.size()) return false;
        equality_pos_++;
        key_values->assign(1, *equality_key_);
        return true;
    }

    while (btree_iter_ && btree_iter_->HasNext()) {
        auto [key, rid] = btree_iter_->Next();
        (void)rid;
        if (lower_bound_.has_value()) {
            int cmp = CompareValues(key, *lower_bound_);
            if (cmp < 0 || (cmp == 0 && !lower_inclusive_)) continue;
        }
        if (upper_bound_.has_value()) {
            int cmp = CompareValues(key, *upper_bound_);
            if (cmp > 0 || (cmp == 0 && !upper_inclusive_)) return false;
        }
        key_values->assign(1, key);
        return true;
    }
    return false;
}

bool IndexOnlyScanExecutor::NextGenericEntry(std::vector<Value> *key_values) {
    if (generic_pos_ >= generic_entries_.size()) return false;
    *key_values = generic_entries_[generic_pos_++].key_values;
    return true;
}

bool IndexOnlyScanExecutor::Next(Tuple *out_tuple) {
    std::vector<Value> key_values;
    bool ok = false;
    if (btree_index_ != nullptr) ok = NextSingleColumnEntry(&key_values);
    else ok = NextGenericEntry(&key_values);
    if (!ok) return false;
    *out_tuple = BuildOutputTuple(key_values);
    return true;
}

void IndexOnlyScanExecutor::Close() {
    equality_hits_.clear();
    equality_pos_ = 0;
    btree_iter_.reset();
    generic_entries_.clear();
    generic_pos_ = 0;
}

const Schema &IndexOnlyScanExecutor::GetOutputSchema() const { return output_schema_; }

}  // namespace simpledb
