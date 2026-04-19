#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../access/btree.h"
#include "../access/generic_btree.h"
#include "../buffer/buffer_pool_manager.h"
#include "../catalog/catalog_manager.h"
#include "../catalog/stats_catalog.h"
#include "../execution/operators.h"
#include "../optimizer/plan_enumerator.h"
#include "../recovery/log_manager.h"
#include "../storage/disk_manager.h"
#include "../storage/relation_manager.h"
#include "../storage/table.h"
#include "binder.h"
#include "parser.h"

namespace simpledb {

struct SqlQueryResult {
    bool has_rows{false};
    Schema schema;
    std::vector<Tuple> rows;
    std::string message;
};

class SqlPlannerFrontend {
public:
    explicit SqlPlannerFrontend(const std::string &db_dir);
    SqlQueryResult ExecuteSql(const std::string &sql_text);

private:
    struct RuntimeRelation {
        std::unique_ptr<HeapFile> heap;
        std::unique_ptr<Table> table;
    };

    struct RuntimeIndexBundle {
        std::string index_name;
        RelationId relation_id{0};
        std::unique_ptr<BTreeIndex> btree;
        std::unique_ptr<BTreeIndexAdapter> btree_adapter;
        std::unique_ptr<GenericBTreeIndex> generic_btree;
    };

    struct SelectBuildInfo {
        std::unique_ptr<AbstractExecutor> executor;
        std::shared_ptr<PhysicalPath> access_path;
        bool used_stream_aggregate{false};
        bool skipped_order_by_sort{false};
        bool skipped_dedup_sort{false};
        bool used_unique{false};
        bool used_top_n{false};
        bool used_limit{false};
        bool used_late_materialization{false};
        bool used_bitmap_heap_scan{false};
        bool used_index_only_scan{false};
        std::vector<PlanOrderKey> required_source_order;
        std::vector<PlanOrderKey> required_access_order;
        std::vector<PlanOrderKey> dedup_source_order;
        std::vector<PlanOrderKey> group_order;
    };

    SelectBuildInfo BuildSelectExecutor(const BoundSelectStatement &stmt) const;
    SqlQueryResult ExecuteExplain(const BoundExplainStatement &stmt);

    void BootstrapFromCatalog();
    void OpenRuntimeRelation(const RelationCatalogEntry &rel);
    void OpenRuntimeIndex(RelationId base_relation_id, const IndexCatalogEntry &idx);
    void RefreshStats(RelationId relation_id);

    SqlQueryResult ExecuteCreateTable(const BoundCreateTableStatement &stmt);
    SqlQueryResult ExecuteCreateIndex(const BoundCreateIndexStatement &stmt);
    SqlQueryResult ExecuteInsert(const BoundInsertStatement &stmt);
    SqlQueryResult ExecuteSelect(const BoundSelectStatement &stmt);
    SqlQueryResult ExecuteCompoundSelect(const BoundCompoundSelectStatement &stmt);
    SqlQueryResult ExecuteUpdate(const BoundUpdateStatement &stmt);
    SqlQueryResult ExecuteDelete(const BoundDeleteStatement &stmt);

    RuntimeRelation &GetRuntimeRelation(RelationId relation_id);
    const RuntimeRelation &GetRuntimeRelation(RelationId relation_id) const;

    std::optional<PlanOrderKey> AbsoluteSourceIndexToOrderKey(const BoundSelectStatement &stmt,
                                                              std::size_t absolute_source_idx) const;
    std::vector<PlanOrderKey> BuildRequiredSourceOrder(const BoundSelectStatement &stmt) const;
    std::vector<PlanOrderKey> BuildDedupSourceOrder(const BoundSelectStatement &stmt) const;
    std::vector<PlanOrderKey> BuildGroupOrder(const BoundSelectStatement &stmt) const;
    bool CanSkipSortAfterStreamAggregate(const BoundSelectStatement &stmt) const;

    DiskManager disk_manager_;
    LogManager log_manager_;
    BufferPoolManager buffer_pool_manager_;
    RelationManager relation_manager_;
    CatalogManager catalog_;
    StatsCatalog stats_catalog_;
    std::unordered_map<RelationId, RuntimeRelation> relations_;
    std::unordered_map<RelationId, std::vector<std::unique_ptr<RuntimeIndexBundle>>> indexes_;
};

}  // namespace simpledb
