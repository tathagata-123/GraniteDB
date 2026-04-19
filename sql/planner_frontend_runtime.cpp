#include "planner_frontend_helpers.h"

#include "lexer.h"
#include "../access/heap_file_iterator.h"

namespace simpledb {
using namespace planner_frontend_helpers;

// Runtime bootstrap and catalog wiring for the SQL frontend.

SqlPlannerFrontend::SqlPlannerFrontend(const std::string &db_dir)
    : disk_manager_(db_dir),
      log_manager_(db_dir + "/wal.log"),
      buffer_pool_manager_(64, &disk_manager_, &log_manager_),
      relation_manager_(&disk_manager_, &buffer_pool_manager_),
      catalog_(db_dir + "/catalog.txt"),
      stats_catalog_(db_dir + "/stats.txt") {
    if (catalog_.Load()) {
        BootstrapFromCatalog();
    }
    stats_catalog_.Load();
    for (const auto &[rel_id, _] : catalog_.GetAllRelations()) {
        if (!stats_catalog_.HasRelationStats(rel_id)) RefreshStats(rel_id);
    }
}

void SqlPlannerFrontend::BootstrapFromCatalog() {
    std::vector<RelationId> relation_ids;
    RelationId max_seen_relation_id = 0;

    for (const auto &[rel_id, rel] : catalog_.GetAllRelations()) {
        relation_ids.push_back(rel_id);
        if (rel_id > max_seen_relation_id) max_seen_relation_id = rel_id;
        for (const auto &idx : rel.indexes) {
            if (idx.index_relation_id > max_seen_relation_id) max_seen_relation_id = idx.index_relation_id;
        }
    }

    for (RelationId rel_id : relation_ids) {
        const RelationCatalogEntry &rel = catalog_.GetRelation(rel_id);
        OpenRuntimeRelation(rel);
    }
    for (RelationId rel_id : relation_ids) {
        const RelationCatalogEntry &rel = catalog_.GetRelation(rel_id);
        for (const auto &idx : rel.indexes) OpenRuntimeIndex(rel_id, idx);
    }

    relation_manager_.EnsureNextRelationIdAtLeast(max_seen_relation_id + 1);
}

void SqlPlannerFrontend::OpenRuntimeRelation(const RelationCatalogEntry &rel) {
    relation_manager_.RegisterExistingHeapRelation(
        rel.relation_id,
        rel.relation_name,
        rel.schema,
        rel.heap_file_name.empty() ? (rel.relation_name + ".heap") : rel.heap_file_name);
    relation_manager_.BuildFreeSpaceMap(rel.relation_id);

    RuntimeRelation runtime;
    runtime.heap = std::make_unique<HeapFile>(
        &buffer_pool_manager_,
        rel.relation_id,
        rel.schema,
        relation_manager_.GetFreeSpaceMap(rel.relation_id),
        &log_manager_,
        nullptr);
    catalog_.AttachHeapFile(rel.relation_id, runtime.heap.get());
    runtime.table = std::make_unique<Table>(&catalog_, rel.relation_id);
    relations_[rel.relation_id] = std::move(runtime);
}

void SqlPlannerFrontend::OpenRuntimeIndex(RelationId base_relation_id, const IndexCatalogEntry &idx) {
    std::string index_file_name = idx.index_file_name.empty() ? (idx.index_name + ".idx") : idx.index_file_name;
    disk_manager_.OpenRelation(idx.index_relation_id, index_file_name);

    auto bundle = std::make_unique<RuntimeIndexBundle>();
    bundle->index_name = idx.index_name;
    bundle->relation_id = idx.index_relation_id;

    if (idx.key_columns.size() == 1) {
        const auto &key_col = idx.key_columns[0];
        bundle->btree = std::make_unique<BTreeIndex>(
            &buffer_pool_manager_, idx.index_relation_id, key_col.type, key_col.max_varchar_len, &log_manager_);
        bundle->btree_adapter = std::make_unique<BTreeIndexAdapter>(bundle->btree.get());
        catalog_.AttachIndex(base_relation_id, idx.index_name, bundle->btree_adapter.get());
    } else {
        IndexDefinition def;
        def.index_name = idx.index_name;
        def.base_relation_id = idx.base_relation_id;
        def.index_relation_id = idx.index_relation_id;
        def.key_columns = idx.key_columns;
        def.is_unique = idx.is_unique;
        def.null_policy = idx.null_policy;
        def.kind = idx.kind;
        def.root_page_no = idx.root_page_no;
        bundle->generic_btree = std::make_unique<GenericBTreeIndex>(&buffer_pool_manager_, def, &log_manager_);
        catalog_.AttachIndex(base_relation_id, idx.index_name, bundle->generic_btree.get());
    }

    indexes_[base_relation_id].push_back(std::move(bundle));
}

void SqlPlannerFrontend::RefreshStats(RelationId relation_id) {
    if (catalog_.GetRelation(relation_id).heap_file != nullptr) {
        stats_catalog_.AnalyzeRelation(catalog_, relation_id);
    }
}

SqlPlannerFrontend::RuntimeRelation &SqlPlannerFrontend::GetRuntimeRelation(RelationId relation_id) {
    auto it = relations_.find(relation_id);
    if (it == relations_.end()) throw std::runtime_error("Runtime relation not found");
    return it->second;
}

const SqlPlannerFrontend::RuntimeRelation &SqlPlannerFrontend::GetRuntimeRelation(RelationId relation_id) const {
    auto it = relations_.find(relation_id);
    if (it == relations_.end()) throw std::runtime_error("Runtime relation not found");
    return it->second;
}

SqlQueryResult SqlPlannerFrontend::ExecuteSql(const std::string &sql_text) {
    SqlLexer lexer(sql_text);
    std::vector<SqlToken> tokens = lexer.Tokenize();
    SqlParser parser(std::move(tokens));
    std::unique_ptr<StatementAST> statement = parser.ParseStatement();
    SqlBinder binder(catalog_);
    BoundStatement bound = binder.Bind(*statement);

    if (std::holds_alternative<BoundCreateTableStatement>(bound)) return ExecuteCreateTable(std::get<BoundCreateTableStatement>(bound));
    if (std::holds_alternative<BoundCreateIndexStatement>(bound)) return ExecuteCreateIndex(std::get<BoundCreateIndexStatement>(bound));
    if (std::holds_alternative<BoundInsertStatement>(bound)) return ExecuteInsert(std::get<BoundInsertStatement>(bound));
    if (std::holds_alternative<BoundSelectStatement>(bound)) return ExecuteSelect(std::get<BoundSelectStatement>(bound));
    if (std::holds_alternative<BoundCompoundSelectStatement>(bound)) return ExecuteCompoundSelect(std::get<BoundCompoundSelectStatement>(bound));
    if (std::holds_alternative<BoundUpdateStatement>(bound)) return ExecuteUpdate(std::get<BoundUpdateStatement>(bound));
    if (std::holds_alternative<BoundDeleteStatement>(bound)) return ExecuteDelete(std::get<BoundDeleteStatement>(bound));
    if (std::holds_alternative<BoundExplainStatement>(bound)) return ExecuteExplain(std::get<BoundExplainStatement>(bound));
    throw std::runtime_error("Unknown bound statement");
}

SqlQueryResult SqlPlannerFrontend::ExecuteCreateTable(const BoundCreateTableStatement &stmt) {
    RelationId relation_id = relation_manager_.CreateHeapRelation(stmt.table_name, stmt.schema, stmt.table_name + ".heap");
    RuntimeRelation runtime;
    runtime.heap = std::make_unique<HeapFile>(&buffer_pool_manager_, relation_id, stmt.schema, relation_manager_.GetFreeSpaceMap(relation_id), &log_manager_, nullptr);
    catalog_.RegisterRelation(relation_id, stmt.table_name, stmt.schema, runtime.heap.get(), stmt.table_name + ".heap");
    runtime.table = std::make_unique<Table>(&catalog_, relation_id);
    relations_[relation_id] = std::move(runtime);
    RefreshStats(relation_id);
    return SqlQueryResult{false, Schema(), {}, "Created table " + stmt.table_name + "."};
}

SqlQueryResult SqlPlannerFrontend::ExecuteCreateIndex(const BoundCreateIndexStatement &stmt) {
    const RelationCatalogEntry &rel = catalog_.GetRelation(stmt.relation_id);
    RuntimeRelation &runtime_rel = GetRuntimeRelation(stmt.relation_id);
    RelationId index_relation_id = relation_manager_.AllocateRelationId();
    std::string file_name = stmt.index_name + ".idx";
    disk_manager_.CreateRelation(index_relation_id, file_name);

    auto bundle = std::make_unique<RuntimeIndexBundle>();
    bundle->index_name = stmt.index_name;
    bundle->relation_id = index_relation_id;

    IndexCatalogEntry entry;
    entry.index_name = stmt.index_name;
    entry.index_file_name = file_name;
    entry.base_relation_id = rel.relation_id;
    entry.index_relation_id = index_relation_id;
    entry.key_columns = stmt.key_columns;
    entry.is_unique = stmt.unique;
    entry.null_policy = NullPolicy::NOT_SUPPORTED;
    entry.kind = IndexKind::BTREE;

    try {
        if (stmt.key_columns.size() == 1) {
            const auto &key_col = stmt.key_columns[0];
            bundle->btree = std::make_unique<BTreeIndex>(&buffer_pool_manager_, index_relation_id, key_col.type, key_col.max_varchar_len, &log_manager_);
            bundle->btree_adapter = std::make_unique<BTreeIndexAdapter>(bundle->btree.get());
            entry.runtime_index = bundle->btree_adapter.get();
            entry.root_page_no = bundle->btree->GetRootPageNo();
        } else {
            IndexDefinition def;
            def.index_name = stmt.index_name;
            def.base_relation_id = rel.relation_id;
            def.index_relation_id = index_relation_id;
            def.key_columns = stmt.key_columns;
            def.is_unique = stmt.unique;
            def.null_policy = NullPolicy::NOT_SUPPORTED;
            def.kind = IndexKind::BTREE;
            bundle->generic_btree = std::make_unique<GenericBTreeIndex>(&buffer_pool_manager_, def, &log_manager_);
            entry.runtime_index = bundle->generic_btree.get();
            entry.root_page_no = bundle->generic_btree->GetRootPageNo();
        }

        HeapFileIterator it(runtime_rel.heap.get());
        while (it.HasNext()) {
            auto [rid, tuple] = it.Next();
            std::vector<Value> key_values = BuildIndexKeyValues(tuple, stmt.key_columns);
            ValidateIndexBuildKeyValues(stmt.index_name, key_values, entry.null_policy);
            if (stmt.unique) {
                std::vector<RID> hits = entry.runtime_index->SearchExact(key_values);
                if (!hits.empty()) {
                    throw std::runtime_error("CREATE UNIQUE INDEX failed: duplicate key encountered for index " + stmt.index_name);
                }
            }
            entry.runtime_index->InsertEntry(key_values, rid);
        }

        catalog_.RegisterIndex(entry);
        relation_manager_.RegisterIndexFile(rel.relation_id,
                                            stmt.index_name,
                                            file_name,
                                            PageId{index_relation_id, entry.root_page_no});
        indexes_[rel.relation_id].push_back(std::move(bundle));
        RefreshStats(rel.relation_id);
        return SqlQueryResult{false, Schema(), {}, "Created index " + stmt.index_name + " on " + stmt.table_name + "."};
    } catch (...) {
        try {
            uint32_t num_pages = disk_manager_.GetNumPages(index_relation_id);
            for (PageNo page_no = 0; page_no < num_pages; ++page_no) {
                buffer_pool_manager_.DeletePage(PageId{index_relation_id, page_no});
            }
        } catch (...) {
        }
        disk_manager_.DestroyRelation(index_relation_id);
        throw;
    }
}

SqlQueryResult SqlPlannerFrontend::ExecuteInsert(const BoundInsertStatement &stmt) {
    RuntimeRelation &runtime = GetRuntimeRelation(stmt.relation_id);
    runtime.table->InsertTuple(stmt.tuple);
    RefreshStats(stmt.relation_id);
    return SqlQueryResult{false, Schema(), {}, "Inserted 1 row."};
}
}  // namespace simpledb
