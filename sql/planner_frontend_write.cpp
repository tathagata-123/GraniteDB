#include "planner_frontend_helpers.h"

#include "../access/heap_file_iterator.h"

namespace simpledb {
using namespace planner_frontend_helpers;

// UPDATE and DELETE planning and execution helpers.

SqlQueryResult SqlPlannerFrontend::ExecuteUpdate(const BoundUpdateStatement &stmt) {
    RuntimeRelation &runtime = GetRuntimeRelation(stmt.relation_id);
    const Schema &schema = runtime.heap->GetSchema();
    std::vector<std::pair<RID, Tuple>> updates;
    HeapFileIterator it(runtime.heap.get());
    while (it.HasNext()) {
        auto [rid, old_tuple] = it.Next();
        bool matches = true;
        if (stmt.predicate != nullptr) matches = ValueAsBool(stmt.predicate->Evaluate(&old_tuple, &schema, nullptr, nullptr));
        if (!matches) continue;
        std::vector<Value> new_values = old_tuple.GetValues();
        for (const auto &assignment : stmt.assignments) new_values[assignment.column_idx] = assignment.value;
        updates.push_back({rid, Tuple(std::move(new_values))});
    }
    std::size_t updated = 0;
    for (auto &entry : updates) {
        RID ignored{};
        if (runtime.table->UpdateTuple(entry.first, entry.second, &ignored)) updated++;
    }
    RefreshStats(stmt.relation_id);
    return SqlQueryResult{false, Schema(), {}, RowsMessage("Updated", updated)};
}

SqlQueryResult SqlPlannerFrontend::ExecuteDelete(const BoundDeleteStatement &stmt) {
    RuntimeRelation &runtime = GetRuntimeRelation(stmt.relation_id);
    const Schema &schema = runtime.heap->GetSchema();
    std::vector<RID> delete_rids;
    HeapFileIterator it(runtime.heap.get());
    while (it.HasNext()) {
        auto [rid, tuple] = it.Next();
        bool matches = true;
        if (stmt.predicate != nullptr) matches = ValueAsBool(stmt.predicate->Evaluate(&tuple, &schema, nullptr, nullptr));
        if (matches) delete_rids.push_back(rid);
    }
    std::size_t deleted = 0;
    for (const RID &rid : delete_rids) if (runtime.table->DeleteTuple(rid)) deleted++;
    RefreshStats(stmt.relation_id);
    return SqlQueryResult{false, Schema(), {}, RowsMessage("Deleted", deleted)};
}
}  // namespace simpledb
