#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../common/schema.h"
#include "../common/types.h"
#include "../common/value.h"
#include "../execution/expressions.h"

namespace simpledb {

enum class LogicalNodeType {
    GET,
    FILTER,
    PROJECT,
    JOIN,
    SORT,
    AGGREGATE,
    INSERT,
    UPDATE,
    DELETE_OP
};

enum class LogicalAggregateType {
    COUNT,
    SUM,
    MIN,
    MAX
};

struct LogicalSortKey {
    std::unique_ptr<AbstractExpression> expr;
    bool ascending{true};
};

class LogicalPlanNode {
public:
    LogicalPlanNode(LogicalNodeType type, Schema output_schema)
        : type_(type), output_schema_(std::move(output_schema)) {}
    virtual ~LogicalPlanNode() = default;

    LogicalNodeType GetType() const { return type_; }
    const Schema &GetOutputSchema() const { return output_schema_; }

protected:
    LogicalNodeType type_;
    Schema output_schema_;
};

class LogicalGetNode : public LogicalPlanNode {
public:
    LogicalGetNode(RelationId relation_id, Schema output_schema)
        : LogicalPlanNode(LogicalNodeType::GET, std::move(output_schema)),
          relation_id_(relation_id) {}

    RelationId GetRelationId() const { return relation_id_; }

private:
    RelationId relation_id_;
};

class LogicalFilterNode : public LogicalPlanNode {
public:
    LogicalFilterNode(std::unique_ptr<LogicalPlanNode> child,
                      std::unique_ptr<AbstractExpression> predicate,
                      Schema output_schema)
        : LogicalPlanNode(LogicalNodeType::FILTER, std::move(output_schema)),
          child_(std::move(child)),
          predicate_(std::move(predicate)) {}

    const LogicalPlanNode *GetChild() const { return child_.get(); }
    const AbstractExpression *GetPredicate() const { return predicate_.get(); }

private:
    std::unique_ptr<LogicalPlanNode> child_;
    std::unique_ptr<AbstractExpression> predicate_;
};

class LogicalProjectNode : public LogicalPlanNode {
public:
    LogicalProjectNode(std::unique_ptr<LogicalPlanNode> child,
                       std::vector<std::unique_ptr<AbstractExpression>> projections,
                       Schema output_schema)
        : LogicalPlanNode(LogicalNodeType::PROJECT, std::move(output_schema)),
          child_(std::move(child)),
          projections_(std::move(projections)) {}

    const LogicalPlanNode *GetChild() const { return child_.get(); }
    const std::vector<std::unique_ptr<AbstractExpression>> &GetProjections() const { return projections_; }

private:
    std::unique_ptr<LogicalPlanNode> child_;
    std::vector<std::unique_ptr<AbstractExpression>> projections_;
};

class LogicalJoinNode : public LogicalPlanNode {
public:
    LogicalJoinNode(std::unique_ptr<LogicalPlanNode> left,
                    std::unique_ptr<LogicalPlanNode> right,
                    std::unique_ptr<AbstractExpression> predicate,
                    Schema output_schema)
        : LogicalPlanNode(LogicalNodeType::JOIN, std::move(output_schema)),
          left_(std::move(left)),
          right_(std::move(right)),
          predicate_(std::move(predicate)) {}

    const LogicalPlanNode *GetLeft() const { return left_.get(); }
    const LogicalPlanNode *GetRight() const { return right_.get(); }
    const AbstractExpression *GetPredicate() const { return predicate_.get(); }

private:
    std::unique_ptr<LogicalPlanNode> left_;
    std::unique_ptr<LogicalPlanNode> right_;
    std::unique_ptr<AbstractExpression> predicate_;
};

class LogicalSortNode : public LogicalPlanNode {
public:
    LogicalSortNode(std::unique_ptr<LogicalPlanNode> child,
                    std::vector<LogicalSortKey> sort_keys,
                    Schema output_schema)
        : LogicalPlanNode(LogicalNodeType::SORT, std::move(output_schema)),
          child_(std::move(child)),
          sort_keys_(std::move(sort_keys)) {}

    const LogicalPlanNode *GetChild() const { return child_.get(); }
    const std::vector<LogicalSortKey> &GetSortKeys() const { return sort_keys_; }

private:
    std::unique_ptr<LogicalPlanNode> child_;
    std::vector<LogicalSortKey> sort_keys_;
};

class LogicalAggregateNode : public LogicalPlanNode {
public:
    LogicalAggregateNode(std::unique_ptr<LogicalPlanNode> child,
                         std::vector<std::unique_ptr<AbstractExpression>> group_by_exprs,
                         std::vector<LogicalAggregateType> agg_types,
                         std::vector<std::unique_ptr<AbstractExpression>> agg_exprs,
                         Schema output_schema)
        : LogicalPlanNode(LogicalNodeType::AGGREGATE, std::move(output_schema)),
          child_(std::move(child)),
          group_by_exprs_(std::move(group_by_exprs)),
          agg_types_(std::move(agg_types)),
          agg_exprs_(std::move(agg_exprs)) {}

    const LogicalPlanNode *GetChild() const { return child_.get(); }
    const std::vector<std::unique_ptr<AbstractExpression>> &GetGroupByExprs() const { return group_by_exprs_; }
    const std::vector<LogicalAggregateType> &GetAggTypes() const { return agg_types_; }
    const std::vector<std::unique_ptr<AbstractExpression>> &GetAggExprs() const { return agg_exprs_; }

private:
    std::unique_ptr<LogicalPlanNode> child_;
    std::vector<std::unique_ptr<AbstractExpression>> group_by_exprs_;
    std::vector<LogicalAggregateType> agg_types_;
    std::vector<std::unique_ptr<AbstractExpression>> agg_exprs_;
};

class LogicalInsertNode : public LogicalPlanNode {
public:
    LogicalInsertNode(RelationId relation_id, std::vector<Value> values, Schema output_schema = Schema())
        : LogicalPlanNode(LogicalNodeType::INSERT, std::move(output_schema)),
          relation_id_(relation_id),
          values_(std::move(values)) {}

    RelationId GetRelationId() const { return relation_id_; }
    const std::vector<Value> &GetValues() const { return values_; }

private:
    RelationId relation_id_;
    std::vector<Value> values_;
};

class LogicalUpdateNode : public LogicalPlanNode {
public:
    struct Assignment {
        std::size_t column_idx{0};
        Value value;
    };

    LogicalUpdateNode(RelationId relation_id,
                      std::vector<Assignment> assignments,
                      std::unique_ptr<AbstractExpression> predicate,
                      Schema output_schema = Schema())
        : LogicalPlanNode(LogicalNodeType::UPDATE, std::move(output_schema)),
          relation_id_(relation_id),
          assignments_(std::move(assignments)),
          predicate_(std::move(predicate)) {}

    RelationId GetRelationId() const { return relation_id_; }
    const std::vector<Assignment> &GetAssignments() const { return assignments_; }
    const AbstractExpression *GetPredicate() const { return predicate_.get(); }

private:
    RelationId relation_id_;
    std::vector<Assignment> assignments_;
    std::unique_ptr<AbstractExpression> predicate_;
};

class LogicalDeleteNode : public LogicalPlanNode {
public:
    LogicalDeleteNode(RelationId relation_id,
                      std::unique_ptr<AbstractExpression> predicate,
                      Schema output_schema = Schema())
        : LogicalPlanNode(LogicalNodeType::DELETE_OP, std::move(output_schema)),
          relation_id_(relation_id),
          predicate_(std::move(predicate)) {}

    RelationId GetRelationId() const { return relation_id_; }
    const AbstractExpression *GetPredicate() const { return predicate_.get(); }

private:
    RelationId relation_id_;
    std::unique_ptr<AbstractExpression> predicate_;
};

}  // namespace simpledb
