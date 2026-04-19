#include "planner_frontend_helpers.h"

namespace simpledb {
using namespace planner_frontend_helpers;

// SELECT planning, access-path selection, executor assembly, compound set operations, and EXPLAIN output.

std::optional<PlanOrderKey> SqlPlannerFrontend::AbsoluteSourceIndexToOrderKey(
    const BoundSelectStatement &stmt,
    std::size_t absolute_source_idx) const {
    for (const auto &table : stmt.tables) {
        std::size_t start = table.offset;
        std::size_t end = table.offset + table.relation->schema.GetColumnCount();
        if (absolute_source_idx >= start && absolute_source_idx < end) {
            return PlanOrderKey{table.relation_id, absolute_source_idx - start, true};
        }
    }
    return std::nullopt;
}

std::vector<PlanOrderKey> SqlPlannerFrontend::BuildGroupOrder(const BoundSelectStatement &stmt) const {
    std::vector<PlanOrderKey> out;
    for (const auto &expr : stmt.group_by_exprs) {
        auto *col = dynamic_cast<ColumnValueExpression *>(expr.get());
        if (col == nullptr || col->GetSide() != TupleSide::SINGLE) return {};
        auto key = AbsoluteSourceIndexToOrderKey(stmt, col->GetColumnIdx());
        if (!key.has_value()) return {};
        out.push_back(*key);
    }
    return out;
}

std::vector<PlanOrderKey> SqlPlannerFrontend::BuildRequiredSourceOrder(const BoundSelectStatement &stmt) const {
    if (stmt.has_aggregate || stmt.order_by_on_output) return {};
    std::vector<PlanOrderKey> out;
    for (const auto &sort_key : stmt.order_by) {
        if (!sort_key.ascending) return {};
        auto *order_col = dynamic_cast<ColumnValueExpression *>(sort_key.expr.get());
        if (order_col == nullptr || order_col->GetSide() != TupleSide::SINGLE) return {};
        auto key = AbsoluteSourceIndexToOrderKey(stmt, order_col->GetColumnIdx());
        if (!key.has_value()) return {};
        out.push_back(*key);
    }
    return out;
}

std::vector<PlanOrderKey> SqlPlannerFrontend::BuildDedupSourceOrder(const BoundSelectStatement &stmt) const {
    if (!stmt.requires_dedup || stmt.has_aggregate) return {};
    std::vector<PlanOrderKey> out;
    out.reserve(stmt.project_exprs.size());
    for (const auto &expr : stmt.project_exprs) {
        auto *col = dynamic_cast<ColumnValueExpression *>(expr.get());
        if (col == nullptr || col->GetSide() != TupleSide::SINGLE) return {};
        auto key = AbsoluteSourceIndexToOrderKey(stmt, col->GetColumnIdx());
        if (!key.has_value()) return {};
        out.push_back(*key);
    }
    return out;
}

bool SqlPlannerFrontend::CanSkipSortAfterStreamAggregate(const BoundSelectStatement &stmt) const {
    if (stmt.order_by.empty()) return true;
    std::size_t group_count = stmt.group_by_exprs.size();
    for (std::size_t i = 0; i < stmt.order_by.size(); i++) {
        if (!stmt.order_by[i].ascending) return false;
        auto *order_col = dynamic_cast<ColumnValueExpression *>(stmt.order_by[i].expr.get());
        if (order_col == nullptr || order_col->GetSide() != TupleSide::SINGLE) return false;
        std::size_t output_idx = order_col->GetColumnIdx();
        if (output_idx >= stmt.project_exprs.size()) return false;
        auto *project_col = dynamic_cast<ColumnValueExpression *>(stmt.project_exprs[output_idx].get());
        if (project_col == nullptr || project_col->GetSide() != TupleSide::SINGLE) return false;
        if (project_col->GetColumnIdx() != i || project_col->GetColumnIdx() >= group_count) return false;
    }
    return true;
}

SqlPlannerFrontend::SelectBuildInfo SqlPlannerFrontend::BuildSelectExecutor(const BoundSelectStatement &stmt) const {
    SelectBuildInfo build;
    build.required_source_order = BuildRequiredSourceOrder(stmt);
    build.dedup_source_order = BuildDedupSourceOrder(stmt);
    build.group_order = BuildGroupOrder(stmt);
    if (stmt.has_aggregate && !build.group_order.empty() && CanSkipSortAfterStreamAggregate(stmt)) {
        build.required_access_order = build.group_order;
    } else {
        build.required_access_order = !build.required_source_order.empty() ? build.required_source_order : build.dedup_source_order;
    }

    PlanEnumerator optimizer(catalog_, stats_catalog_);
    build.access_path = optimizer.OptimizeLogicalPlan(
        *stmt.logical_plan,
        build.required_access_order,
        build.group_order,
        stmt.has_aggregate);

    build.skipped_order_by_sort = stmt.order_by.empty();
    build.skipped_dedup_sort = stmt.requires_dedup && !build.dedup_source_order.empty() &&
                               build.access_path->SatisfiesOrder(build.dedup_source_order);
    if (!stmt.order_by.empty()) {
        if (!stmt.has_aggregate && !stmt.order_by_on_output) {
            build.skipped_order_by_sort = !build.required_source_order.empty() &&
                                          build.access_path->SatisfiesOrder(build.required_source_order);
        } else if (stmt.has_aggregate) {
            build.skipped_order_by_sort = CanSkipSortAfterStreamAggregate(stmt);
        } else {
            build.skipped_order_by_sort = false;
        }
    }

    bool single_base_table =
        stmt.tables.size() == 1 && !stmt.has_join &&
        build.access_path->kind == PhysicalPath::Kind::BASE &&
        build.access_path->relation_id == stmt.tables[0].relation_id;

    bool can_use_late_materialization =
        single_base_table &&
        !stmt.has_aggregate &&
        !stmt.requires_dedup &&
        stmt.limit.has_value() &&
        build.access_path->access_index != nullptr &&
        build.access_path->access_spec.residual_predicates.empty() &&
        (stmt.order_by.empty() || build.skipped_order_by_sort);

    const RuntimeRelation *single_runtime = single_base_table ? &GetRuntimeRelation(stmt.tables[0].relation_id) : nullptr;
    auto referenced_cols = single_base_table ? CollectReferencedSourceColumns(stmt) : std::unordered_set<std::size_t>{};

    bool can_use_index_only = false;
    if (single_base_table && !stmt.has_aggregate && build.access_path->access_index != nullptr) {
        std::unordered_map<std::size_t, std::size_t> key_pos;
        for (std::size_t i = 0; i < build.access_path->access_index->key_columns.size(); i++) {
            key_pos[build.access_path->access_index->key_columns[i].column_idx] = i;
        }
        can_use_index_only = true;
        for (std::size_t col_idx : referenced_cols) {
            if (key_pos.count(col_idx) == 0) {
                can_use_index_only = false;
                break;
            }
        }
    }

    std::unique_ptr<AbstractExecutor> root;
    bool needs_full_predicate_recheck = false;
    if (can_use_late_materialization && !can_use_index_only) {
        std::vector<RID> rids = build.access_path->CollectBaseRids(catalog_);
        root = std::make_unique<RidListExecutor>(std::move(rids));
        root = std::make_unique<LimitExecutor>(std::move(root), *stmt.limit);
        build.used_limit = true;
        build.used_late_materialization = true;

        root = std::make_unique<MaterializeRidsExecutor>(std::move(root), single_runtime->heap.get());

        if (!IsIdentityProjection(stmt.project_exprs, root->GetOutputSchema())) {
            auto project_exprs = CloneExprVector(stmt.project_exprs);
            root = std::make_unique<ProjectExecutor>(std::move(root), std::move(project_exprs), stmt.output_schema);
        }

        build.executor = std::move(root);
        return build;
    }

    if (can_use_index_only) {
        std::unordered_map<std::size_t, std::size_t> key_pos;
        for (std::size_t i = 0; i < build.access_path->access_index->key_columns.size(); i++) {
            key_pos[build.access_path->access_index->key_columns[i].column_idx] = i;
        }
        if (build.access_path->access_index->GetBTreeIndex() != nullptr) {
            std::optional<Value> equality_key;
            std::optional<Value> lower;
            std::optional<Value> upper;
            bool lower_inclusive = true;
            bool upper_inclusive = true;
            if (build.access_path->access_type == AccessPathType::INDEX_SCAN_EQ && !build.access_path->access_spec.index_predicates.empty()) {
                equality_key = build.access_path->access_spec.index_predicates.front().constant;
            }
            if (build.access_path->access_spec.lower_bound.has_value()) {
                lower = build.access_path->access_spec.lower_bound->constant;
                lower_inclusive = (build.access_path->access_spec.lower_bound->cmp == ComparisonType::GTE);
            }
            if (build.access_path->access_spec.upper_bound.has_value()) {
                upper = build.access_path->access_spec.upper_bound->constant;
                upper_inclusive = (build.access_path->access_spec.upper_bound->cmp == ComparisonType::LTE);
            }
            root = std::make_unique<IndexOnlyScanExecutor>(
                single_runtime->heap->GetSchema(), build.access_path->access_index->GetBTreeIndex(),
                equality_key, lower, upper, lower_inclusive, upper_inclusive, std::move(key_pos));
        } else {
            GenericBTreeIndex::PrefixScanSpec scan_spec;
            for (std::size_t i = 0; i < build.access_path->access_spec.equality_prefix_count && i < build.access_path->access_spec.index_predicates.size(); i++) {
                scan_spec.equality_prefix.push_back(build.access_path->access_spec.index_predicates[i].constant);
            }
            if (build.access_path->access_spec.lower_bound.has_value()) {
                scan_spec.lower_bound = build.access_path->access_spec.lower_bound->constant;
                scan_spec.lower_inclusive = (build.access_path->access_spec.lower_bound->cmp == ComparisonType::GTE);
            }
            if (build.access_path->access_spec.upper_bound.has_value()) {
                scan_spec.upper_bound = build.access_path->access_spec.upper_bound->constant;
                scan_spec.upper_inclusive = (build.access_path->access_spec.upper_bound->cmp == ComparisonType::LTE);
            }
            bool full_scan = build.access_path->access_type == AccessPathType::INDEX_SCAN_ORDERED &&
                             scan_spec.equality_prefix.empty() && !scan_spec.lower_bound.has_value() && !scan_spec.upper_bound.has_value();
            root = std::make_unique<IndexOnlyScanExecutor>(
                single_runtime->heap->GetSchema(), build.access_path->access_index->GetGenericBTreeIndex(),
                std::move(scan_spec), full_scan, std::move(key_pos));
        }
        build.used_index_only_scan = true;
        needs_full_predicate_recheck = (stmt.where_predicate != nullptr);
    } else if (single_base_table && !stmt.has_aggregate && build.required_access_order.empty() && stmt.where_predicate != nullptr) {
        std::vector<std::vector<QueryPredicateSpec>> dnf_groups;
        if (CollectBitmapDnfGroups(stmt.where_predicate.get(), &dnf_groups)) {
            std::vector<std::unique_ptr<AbstractExecutor>> disjunct_children;
            std::size_t total_bitmap_terms = 0;
            for (const auto &group : dnf_groups) {
                std::vector<std::unique_ptr<AbstractExecutor>> group_children;
                bool all_indexable = true;
                for (const auto &pred : group) {
                    const IndexCatalogEntry *index = catalog_.FindIndexOnColumn(stmt.tables[0].relation_id, pred.column_idx);
                    auto leaf = MakeBitmapLeaf(index, pred);
                    if (!leaf) {
                        all_indexable = false;
                        break;
                    }
                    group_children.push_back(std::move(leaf));
                    total_bitmap_terms++;
                }
                if (!all_indexable || group_children.empty()) {
                    disjunct_children.clear();
                    break;
                }
                if (group_children.size() == 1) disjunct_children.push_back(std::move(group_children[0]));
                else disjunct_children.push_back(std::make_unique<BitmapAndExecutor>(std::move(group_children)));
            }
            if (total_bitmap_terms >= 2 && !disjunct_children.empty()) {
                std::unique_ptr<AbstractExecutor> bitmap_root;
                if (disjunct_children.size() == 1) bitmap_root = std::move(disjunct_children[0]);
                else bitmap_root = std::make_unique<BitmapOrExecutor>(std::move(disjunct_children));
                root = std::make_unique<BitmapHeapScanExecutor>(std::move(bitmap_root), single_runtime->heap.get());
                build.used_bitmap_heap_scan = true;
                build.skipped_order_by_sort = stmt.order_by.empty();
                build.skipped_dedup_sort = false;
                needs_full_predicate_recheck = true;
            }
        }
    }

    if (!root) {
        root = build.access_path->BuildExecutor(catalog_);
    }

    if ((stmt.has_join || needs_full_predicate_recheck) && stmt.where_predicate != nullptr) {
        root = std::make_unique<FilterExecutor>(
            std::move(root), std::unique_ptr<AbstractExpression>(stmt.where_predicate->Clone()));
    }

    if (stmt.has_aggregate) {
        build.used_stream_aggregate = !build.group_order.empty() && CanSkipSortAfterStreamAggregate(stmt);
        if (build.used_stream_aggregate && !build.access_path->SatisfiesOrder(build.group_order)) {
            std::vector<SortKeySpec> presort_keys;
            presort_keys.reserve(stmt.group_by_exprs.size());
            for (const auto &expr : stmt.group_by_exprs) {
                SortKeySpec spec;
                spec.expr = std::unique_ptr<AbstractExpression>(expr->Clone());
                spec.ascending = true;
                presort_keys.push_back(std::move(spec));
            }
            root = std::make_unique<SortExecutor>(std::move(root), std::move(presort_keys));
        }
        auto group_exprs = CloneExprVector(stmt.group_by_exprs);
        auto agg_inputs = CloneExprVector(stmt.agg_input_exprs);
        if (build.used_stream_aggregate) {
            root = std::make_unique<StreamAggregateExecutor>(
                std::move(root), std::move(group_exprs), stmt.agg_types, std::move(agg_inputs), stmt.aggregate_schema);
        } else {
            root = std::make_unique<AggregateExecutor>(
                std::move(root), std::move(group_exprs), stmt.agg_types, std::move(agg_inputs), stmt.aggregate_schema);
        }

        if (!IsIdentityProjection(stmt.project_exprs, root->GetOutputSchema())) {
            auto project_exprs = CloneExprVector(stmt.project_exprs);
            root = std::make_unique<ProjectExecutor>(std::move(root), std::move(project_exprs), stmt.output_schema);
        }

        if (stmt.requires_dedup) {
            root = std::make_unique<SortExecutor>(std::move(root), FullRowSortKeys(stmt.output_schema));
            root = std::make_unique<UniqueExecutor>(std::move(root));
            build.used_unique = true;
        }

        if (!stmt.order_by.empty() && !build.skipped_order_by_sort) {
            std::vector<SortKeySpec> sort_keys;
            sort_keys.reserve(stmt.order_by.size());
            for (const auto &key : stmt.order_by) {
                SortKeySpec spec;
                spec.expr = std::unique_ptr<AbstractExpression>(key.expr->Clone());
                spec.ascending = key.ascending;
                sort_keys.push_back(std::move(spec));
            }

            if (stmt.limit.has_value()) {
                root = std::make_unique<TopNExecutor>(std::move(root), std::move(sort_keys), *stmt.limit);
                build.used_top_n = true;
                build.used_limit = true;
            } else {
                root = std::make_unique<SortExecutor>(std::move(root), std::move(sort_keys));
            }
        } else if (stmt.limit.has_value() && !build.used_limit) {
            root = std::make_unique<LimitExecutor>(std::move(root), *stmt.limit);
            build.used_limit = true;
        }

        build.executor = std::move(root);
        return build;
    }

    if (stmt.requires_dedup) {
        if (!IsIdentityProjection(stmt.project_exprs, root->GetOutputSchema())) {
            auto project_exprs = CloneExprVector(stmt.project_exprs);
            root = std::make_unique<ProjectExecutor>(std::move(root), std::move(project_exprs), stmt.output_schema);
        }
        if (!build.skipped_dedup_sort) {
            root = std::make_unique<SortExecutor>(std::move(root), FullRowSortKeys(stmt.output_schema));
        }
        root = std::make_unique<UniqueExecutor>(std::move(root));
        build.used_unique = true;

        if (!stmt.order_by.empty() && !build.skipped_order_by_sort) {
            std::vector<SortKeySpec> sort_keys;
            sort_keys.reserve(stmt.order_by.size());
            for (const auto &key : stmt.order_by) {
                SortKeySpec spec;
                spec.expr = std::unique_ptr<AbstractExpression>(key.expr->Clone());
                spec.ascending = key.ascending;
                sort_keys.push_back(std::move(spec));
            }

            if (stmt.limit.has_value()) {
                root = std::make_unique<TopNExecutor>(std::move(root), std::move(sort_keys), *stmt.limit);
                build.used_top_n = true;
                build.used_limit = true;
            } else {
                root = std::make_unique<SortExecutor>(std::move(root), std::move(sort_keys));
            }
        } else if (stmt.limit.has_value() && !build.used_limit) {
            root = std::make_unique<LimitExecutor>(std::move(root), *stmt.limit);
            build.used_limit = true;
        }

        build.executor = std::move(root);
        return build;
    }

    if (!stmt.order_by.empty() && !build.skipped_order_by_sort) {
        std::vector<SortKeySpec> sort_keys;
        sort_keys.reserve(stmt.order_by.size());
        for (const auto &key : stmt.order_by) {
            SortKeySpec spec;
            spec.expr = std::unique_ptr<AbstractExpression>(key.expr->Clone());
            spec.ascending = key.ascending;
            sort_keys.push_back(std::move(spec));
        }

        if (stmt.limit.has_value()) {
            root = std::make_unique<TopNExecutor>(std::move(root), std::move(sort_keys), *stmt.limit);
            build.used_top_n = true;
            build.used_limit = true;
        } else {
            root = std::make_unique<SortExecutor>(std::move(root), std::move(sort_keys));
        }
    } else if (stmt.limit.has_value() && !build.used_limit) {
        root = std::make_unique<LimitExecutor>(std::move(root), *stmt.limit);
        build.used_limit = true;
    }

    if (!IsIdentityProjection(stmt.project_exprs, root->GetOutputSchema())) {
        auto project_exprs = CloneExprVector(stmt.project_exprs);
        root = std::make_unique<ProjectExecutor>(std::move(root), std::move(project_exprs), stmt.output_schema);
    }

    build.executor = std::move(root);
    return build;
}

SqlQueryResult SqlPlannerFrontend::ExecuteSelect(const BoundSelectStatement &stmt) {
    SqlQueryResult result;
    result.has_rows = true;
    result.schema = stmt.output_schema;
    SelectBuildInfo build = BuildSelectExecutor(stmt);
    build.executor->Init();
    Tuple tuple;
    while (build.executor->Next(&tuple)) result.rows.push_back(tuple);
    build.executor->Close();
    result.message = RowsMessage("Returned", result.rows.size());
    return result;
}

SqlQueryResult SqlPlannerFrontend::ExecuteCompoundSelect(const BoundCompoundSelectStatement &stmt) {
    if (stmt.inputs.empty()) {
        throw std::runtime_error("Compound SELECT requires at least one input");
    }

    std::vector<SelectBuildInfo> builds;
    builds.reserve(stmt.inputs.size());
    for (const auto &input : stmt.inputs) {
        builds.push_back(BuildSelectExecutor(input));
    }

    std::unique_ptr<AbstractExecutor> root = std::move(builds[0].executor);
    for (std::size_t i = 0; i < stmt.operations.size(); i++) {
        auto rhs = std::move(builds[i + 1].executor);
        const auto &op = stmt.operations[i];
        if (op.type == SetOperationType::UNION_OP) {
            std::vector<std::unique_ptr<AbstractExecutor>> children;
            children.push_back(std::move(root));
            children.push_back(std::move(rhs));
            root = std::make_unique<AppendExecutor>(std::move(children), stmt.output_schema);
            if (!op.all) {
                root = std::make_unique<SortExecutor>(std::move(root), FullRowSortKeys(stmt.output_schema));
                root = std::make_unique<UniqueExecutor>(std::move(root));
            }
        } else {
            root = std::make_unique<SortExecutor>(std::move(root), FullRowSortKeys(stmt.output_schema));
            rhs = std::make_unique<SortExecutor>(std::move(rhs), FullRowSortKeys(stmt.output_schema));
            root = std::make_unique<SetOpExecutor>(
                std::move(root), std::move(rhs),
                op.type == SetOperationType::INTERSECT_OP ? SetOpMode::INTERSECT : SetOpMode::EXCEPT,
                op.all,
                stmt.output_schema);
        }
    }

    if (!stmt.order_by.empty()) {
        std::vector<SortKeySpec> sort_keys;
        sort_keys.reserve(stmt.order_by.size());
        for (const auto &key : stmt.order_by) {
            SortKeySpec spec;
            spec.expr = std::unique_ptr<AbstractExpression>(key.expr->Clone());
            spec.ascending = key.ascending;
            sort_keys.push_back(std::move(spec));
        }
        if (stmt.limit.has_value()) root = std::make_unique<TopNExecutor>(std::move(root), std::move(sort_keys), *stmt.limit);
        else root = std::make_unique<SortExecutor>(std::move(root), std::move(sort_keys));
    } else if (stmt.limit.has_value()) {
        root = std::make_unique<LimitExecutor>(std::move(root), *stmt.limit);
    }

    SqlQueryResult result;
    result.has_rows = true;
    result.schema = stmt.output_schema;
    root->Init();
    Tuple tuple;
    while (root->Next(&tuple)) result.rows.push_back(tuple);
    root->Close();
    result.message = RowsMessage("Returned", result.rows.size());
    return result;
}

SqlQueryResult SqlPlannerFrontend::ExecuteExplain(const BoundExplainStatement &stmt) {
    std::ostringstream out;
    if (std::holds_alternative<BoundSelectStatement>(stmt.query)) {
        const auto &select = std::get<BoundSelectStatement>(stmt.query);
        SelectBuildInfo build = BuildSelectExecutor(select);
        out << build.access_path->Explain();
        if (select.has_aggregate) {
            out << "\nAggregateStrategy: " << (build.used_stream_aggregate ? "StreamAggregate" : "HashAggregate");
        }
        if (select.requires_dedup) {
            out << "\nDedup: " << (build.skipped_dedup_sort ? "ordered Unique" : "Sort + Unique");
            if (select.dedup_via_group_by) out << " (GROUP BY without aggregates)";
            else if (select.distinct) out << " (DISTINCT)";
        }
        if (!select.order_by.empty()) {
            if (build.skipped_order_by_sort) out << "\nOrderBy: sort avoided";
            else if (build.used_top_n) out << "\nOrderBy: TopN";
            else out << "\nOrderBy: ExternalMergeSort";
        }
        if (build.used_index_only_scan) out << "\nAccessRefinement: IndexOnlyScan";
        else if (build.used_bitmap_heap_scan) out << "\nAccessRefinement: BitmapIndexScan + BitmapHeapScan";
        if (select.limit.has_value()) {
            out << "\nLimit: " << *select.limit;
            if (build.used_late_materialization) out << " (late materialization)";
        }
    } else {
        const auto &compound = std::get<BoundCompoundSelectStatement>(stmt.query);
        out << "CompoundSetOp";
        for (std::size_t i = 0; i < compound.operations.size(); i++) {
            out << "\n  Input " << i << ": " << BuildSelectExecutor(compound.inputs[i]).access_path->Explain();
            out << "\n  Op " << i << ": ";
            if (compound.operations[i].type == SetOperationType::UNION_OP) out << (compound.operations[i].all ? "Append(UNION ALL)" : "Append + Sort + Unique");
            else if (compound.operations[i].type == SetOperationType::INTERSECT_OP) out << (compound.operations[i].all ? "Sort + SetOpMerge(INTERSECT ALL)" : "Sort + SetOpMerge(INTERSECT)");
            else out << (compound.operations[i].all ? "Sort + SetOpMerge(EXCEPT ALL)" : "Sort + SetOpMerge(EXCEPT)");
        }
        out << "\n  Input " << compound.inputs.size() - 1 << ": " << BuildSelectExecutor(compound.inputs.back()).access_path->Explain();
        if (!compound.order_by.empty()) out << "\nOrderBy: " << (compound.limit.has_value() ? "TopN" : "ExternalMergeSort");
        else if (compound.limit.has_value()) out << "\nLimit: " << *compound.limit;
    }

    SqlQueryResult result;
    result.has_rows = true;
    result.schema = Schema({Column("plan", TypeId::VARCHAR, true, 4096)});
    std::istringstream lines(out.str());
    std::string line;
    while (std::getline(lines, line)) result.rows.emplace_back(std::vector<Value>{Value(line)});
    result.message = RowsMessage("Explained", result.rows.size());
    return result;
}
}  // namespace simpledb
