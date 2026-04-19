// Filter, projection, append, duplicate-elimination, and set operators.

#include "operators_common.h"

#include <algorithm>
#include <stdexcept>

namespace simpledb {
using namespace execution_detail;

FilterExecutor::FilterExecutor(std::unique_ptr<AbstractExecutor> child,
                               std::unique_ptr<AbstractExpression> predicate)
    : child_(std::move(child)), predicate_(std::move(predicate)) {}

void FilterExecutor::Init() { child_->Init(); }

bool FilterExecutor::Next(Tuple *out_tuple) {
    Tuple input;
    while (child_->Next(&input)) {
        Value pred = predicate_->Evaluate(&input, &child_->GetOutputSchema(), nullptr, nullptr);
        if (ValueAsBool(pred)) {
            *out_tuple = input;
            return true;
        }
    }
    return false;
}

void FilterExecutor::Close() { child_->Close(); }
const Schema &FilterExecutor::GetOutputSchema() const { return child_->GetOutputSchema(); }

ProjectExecutor::ProjectExecutor(std::unique_ptr<AbstractExecutor> child,
                                 std::vector<std::unique_ptr<AbstractExpression>> projections,
                                 Schema output_schema)
    : child_(std::move(child)), projections_(std::move(projections)), output_schema_(std::move(output_schema)) {}

void ProjectExecutor::Init() { child_->Init(); }

bool ProjectExecutor::Next(Tuple *out_tuple) {
    Tuple input;
    if (!child_->Next(&input)) return false;
    std::vector<Value> vals;
    vals.reserve(projections_.size());
    for (const auto &expr : projections_) {
        vals.push_back(expr->Evaluate(&input, &child_->GetOutputSchema(), nullptr, nullptr));
    }
    *out_tuple = Tuple(std::move(vals));
    return true;
}

void ProjectExecutor::Close() { child_->Close(); }
const Schema &ProjectExecutor::GetOutputSchema() const { return output_schema_; }



AppendExecutor::AppendExecutor(std::vector<std::unique_ptr<AbstractExecutor>> children,
                               Schema output_schema)
    : children_(std::move(children)), output_schema_(std::move(output_schema)), child_idx_(0) {}

void AppendExecutor::Init() {
    child_idx_ = 0;
    if (!children_.empty()) children_[0]->Init();
}

bool AppendExecutor::Next(Tuple *out_tuple) {
    while (child_idx_ < children_.size()) {
        if (children_[child_idx_]->Next(out_tuple)) return true;
        children_[child_idx_]->Close();
        child_idx_++;
        if (child_idx_ < children_.size()) children_[child_idx_]->Init();
    }
    return false;
}

void AppendExecutor::Close() {
    for (auto &child : children_) child->Close();
    child_idx_ = 0;
}

const Schema &AppendExecutor::GetOutputSchema() const { return output_schema_; }

UniqueExecutor::UniqueExecutor(std::unique_ptr<AbstractExecutor> child)
    : child_(std::move(child)), has_prev_(false) {}

void UniqueExecutor::Init() {
    child_->Init();
    has_prev_ = false;
}

bool UniqueExecutor::Next(Tuple *out_tuple) {
    Tuple tuple;
    while (child_->Next(&tuple)) {
        if (!has_prev_ || CompareTuplesBySchema(prev_tuple_, tuple, child_->GetOutputSchema()) != 0) {
            prev_tuple_ = tuple;
            has_prev_ = true;
            *out_tuple = tuple;
            return true;
        }
    }
    return false;
}

void UniqueExecutor::Close() {
    child_->Close();
    has_prev_ = false;
}

const Schema &UniqueExecutor::GetOutputSchema() const { return child_->GetOutputSchema(); }

SetOpExecutor::SetOpExecutor(std::unique_ptr<AbstractExecutor> left_child,
                             std::unique_ptr<AbstractExecutor> right_child,
                             SetOpMode mode,
                             bool all,
                             Schema output_schema)
    : left_child_(std::move(left_child)), right_child_(std::move(right_child)), mode_(mode), all_(all),
      output_schema_(std::move(output_schema)), left_has_pending_(false), right_has_pending_(false),
      has_left_group_(false), has_right_group_(false), current_left_count_(0), current_right_count_(0),
      emit_ready_(false), emit_remaining_(0) {}

void SetOpExecutor::Init() {
    left_child_->Init();
    right_child_->Init();
    left_has_pending_ = false;
    right_has_pending_ = false;
    emit_ready_ = false;
    emit_remaining_ = 0;
    AdvanceLeftGroup();
    AdvanceRightGroup();
}

void SetOpExecutor::AdvanceLeftGroup() {
    has_left_group_ = ReadNextGroup(left_child_.get(), &current_left_group_, &current_left_count_, &left_has_pending_, &left_pending_tuple_);
}

void SetOpExecutor::AdvanceRightGroup() {
    has_right_group_ = ReadNextGroup(right_child_.get(), &current_right_group_, &current_right_count_, &right_has_pending_, &right_pending_tuple_);
}

bool SetOpExecutor::ReadNextGroup(AbstractExecutor *child,
                                  Tuple *group_value,
                                  std::size_t *group_count,
                                  bool *has_pending,
                                  Tuple *pending_tuple) {
    Tuple tuple;
    if (*has_pending) {
        tuple = *pending_tuple;
        *has_pending = false;
    } else {
        if (!child->Next(&tuple)) return false;
    }

    *group_value = tuple;
    *group_count = 1;
    while (child->Next(&tuple)) {
        if (CompareTuplesBySchema(*group_value, tuple, output_schema_) != 0) {
            *pending_tuple = tuple;
            *has_pending = true;
            break;
        }
        (*group_count)++;
    }
    return true;
}

bool SetOpExecutor::Next(Tuple *out_tuple) {
    if (emit_ready_ && emit_remaining_ > 0) {
        *out_tuple = emit_tuple_;
        emit_remaining_--;
        return true;
    }
    emit_ready_ = false;

    while (has_left_group_) {
        if (!has_right_group_) {
            if (mode_ != SetOpMode::EXCEPT) return false;
            emit_tuple_ = current_left_group_;
            emit_remaining_ = all_ ? current_left_count_ : 1;
            emit_ready_ = emit_remaining_ > 0;
            AdvanceLeftGroup();
            if (emit_ready_) {
                *out_tuple = emit_tuple_;
                emit_remaining_--;
                return true;
            }
            continue;
        }

        int cmp = CompareTuplesBySchema(current_left_group_, current_right_group_, output_schema_);
        if (cmp < 0) {
            if (mode_ == SetOpMode::EXCEPT) {
                emit_tuple_ = current_left_group_;
                emit_remaining_ = all_ ? current_left_count_ : 1;
                emit_ready_ = emit_remaining_ > 0;
                AdvanceLeftGroup();
                if (emit_ready_) {
                    *out_tuple = emit_tuple_;
                    emit_remaining_--;
                    return true;
                }
                continue;
            }
            AdvanceLeftGroup();
            continue;
        }
        if (cmp > 0) {
            AdvanceRightGroup();
            continue;
        }

        std::size_t emit_count = 0;
        if (mode_ == SetOpMode::INTERSECT) emit_count = all_ ? std::min(current_left_count_, current_right_count_) : 1;
        else emit_count = all_ ? (current_left_count_ > current_right_count_ ? current_left_count_ - current_right_count_ : 0) : 0;

        Tuple emit_value = current_left_group_;
        AdvanceLeftGroup();
        AdvanceRightGroup();
        if (emit_count > 0) {
            emit_tuple_ = emit_value;
            emit_remaining_ = emit_count;
            emit_ready_ = true;
            *out_tuple = emit_tuple_;
            emit_remaining_--;
            return true;
        }
    }
    return false;
}

void SetOpExecutor::Close() {
    left_child_->Close();
    right_child_->Close();
    left_has_pending_ = false;
    right_has_pending_ = false;
    has_left_group_ = false;
    has_right_group_ = false;
    current_left_count_ = 0;
    current_right_count_ = 0;
    emit_ready_ = false;
    emit_remaining_ = 0;
}

const Schema &SetOpExecutor::GetOutputSchema() const { return output_schema_; }

}  // namespace simpledb
