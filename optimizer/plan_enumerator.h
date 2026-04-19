#pragma once

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "../catalog/catalog_manager.h"
#include "../catalog/stats_catalog.h"
#include "cost_model.h"
#include "logical_plan.h"
#include "physical_path.h"

namespace simpledb {

struct OptimizerQuery {
    std::vector<RelationId> relations;
    std::vector<QueryPredicateSpec> predicates;
    std::vector<QueryJoinPredicateSpec> joins;
    std::vector<PlanOrderKey> required_order;
    std::vector<PlanOrderKey> group_order;
    bool has_aggregate{false};
    std::optional<std::size_t> limit_count;
};

class PlanEnumerator {
public:
    PlanEnumerator(const CatalogManager &catalog,
                   const StatsCatalog &stats,
                   CostModel cost_model = CostModel());

    std::shared_ptr<PhysicalPath> Optimize(const OptimizerQuery &query) const;
    std::shared_ptr<PhysicalPath> OptimizeLogicalPlan(
        const LogicalPlanNode &logical_root,
        const std::vector<PlanOrderKey> &required_order = {},
        const std::vector<PlanOrderKey> &group_order = {},
        bool has_aggregate = false,
        std::optional<std::size_t> limit_count = std::nullopt) const;

private:
    std::vector<QueryPredicateSpec> GetPredicatesForRelation(
        const OptimizerQuery &query,
        RelationId relation_id) const;

    std::vector<QueryJoinPredicateSpec> GetJoinsBetweenSubsetAndRelation(
        const OptimizerQuery &query,
        uint64_t subset_mask,
        RelationId relation_id,
        const std::unordered_map<RelationId, std::size_t> &rel_pos) const;

    std::vector<std::shared_ptr<PhysicalPath>> MakeBaseCandidates(
        RelationId relation_id,
        uint64_t relation_bit,
        const std::vector<QueryPredicateSpec> &predicates,
        const OptimizerQuery &query) const;

    std::vector<std::shared_ptr<PhysicalPath>> TryBuildJoinPaths(
        const std::shared_ptr<PhysicalPath> &left_path,
        const std::shared_ptr<PhysicalPath> &right_base_path,
        const QueryJoinPredicateSpec &join_pred,
        const OptimizerQuery &query) const;

    double EstimatePredicateListSelectivity(const RelationStats &stats,
                                            const std::vector<QueryPredicateSpec> &predicates) const;

    void AddPrunedCandidate(std::vector<std::shared_ptr<PhysicalPath>> *bucket,
                            std::shared_ptr<PhysicalPath> candidate,
                            const OptimizerQuery &query) const;

    double ComputeFinalCost(const std::shared_ptr<PhysicalPath> &path,
                            const OptimizerQuery &query) const;

    std::string SerializeOrder(const std::vector<PlanOrderKey> &order) const;

    const CatalogManager &catalog_;
    const StatsCatalog &stats_;
    CostModel cost_model_;
};

}  // namespace simpledb
