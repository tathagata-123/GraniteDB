#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../buffer/buffer_pool_manager.h"
#include "../common/schema.h"
#include "disk_manager.h"
#include "free_space_map.h"

namespace simpledb {

struct IndexMetadata {
    std::string index_name;
    std::string index_file_name;
    PageId root_page_id{0, 0};
};

struct RelationMetadata {
    RelationId relation_id{0};
    std::string relation_name;
    std::string heap_file_name;
    Schema schema;
    std::vector<IndexMetadata> indexes;
};

class RelationManager {
public:
    RelationManager(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager);

    RelationId CreateHeapRelation(const std::string &relation_name,
                                  const Schema &schema,
                                  const std::string &heap_file_name = "");

    void RegisterExistingHeapRelation(RelationId relation_id,
                                      const std::string &relation_name,
                                      const Schema &schema,
                                      const std::string &heap_file_name);

    RelationId AllocateRelationId();
    void EnsureNextRelationIdAtLeast(RelationId min_next_relation_id);

    bool HasRelation(RelationId relation_id) const;
    RelationId GetRelationIdByName(const std::string &relation_name) const;

    const RelationMetadata &GetRelationMetadata(RelationId relation_id) const;
    const RelationMetadata &GetRelationMetadata(const std::string &relation_name) const;

    FreeSpaceMap *GetFreeSpaceMap(RelationId relation_id);
    const FreeSpaceMap *GetFreeSpaceMap(RelationId relation_id) const;

    void RegisterIndexFile(RelationId relation_id,
                           const std::string &index_name,
                           const std::string &index_file_name,
                           PageId root_page_id);

    void UpdateIndexRootPage(RelationId relation_id,
                             const std::string &index_name,
                             PageId root_page_id);

    void BuildFreeSpaceMap(RelationId relation_id);

private:
    RelationMetadata &GetRelationMetadataMutable(RelationId relation_id);

private:
    RelationId next_relation_id_;
    DiskManager *disk_manager_;
    BufferPoolManager *buffer_pool_manager_;

    mutable std::mutex latch_;
    std::unordered_map<RelationId, RelationMetadata> relations_by_id_;
    std::unordered_map<std::string, RelationId> relation_name_to_id_;
    std::unordered_map<RelationId, std::unique_ptr<FreeSpaceMap>> free_space_maps_;
};

}  // namespace simpledb
