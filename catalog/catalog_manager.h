#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "../access/index.h"
#include "../access/generic_btree.h"
#include "../common/schema.h"
#include "../common/types.h"

namespace simpledb {

class HeapFile;
class BTreeIndex;

struct IndexCatalogEntry : public IndexDefinition {
    AbstractIndex *runtime_index{nullptr};
    std::string index_file_name;

    PageNo GetRootPageNo() const {
        return runtime_index != nullptr ? runtime_index->GetRootPageNo() : root_page_no;
    }

    bool MatchesSingleColumn(std::size_t column_idx) const {
        return key_columns.size() == 1 && key_columns[0].column_idx == column_idx;
    }

    TypeId GetSingleKeyType() const {
        if (key_columns.size() != 1) {
            throw std::runtime_error("Index is not single-column");
        }
        return key_columns[0].type;
    }

    uint32_t GetSingleMaxVarcharLen() const {
        if (key_columns.size() != 1) {
            throw std::runtime_error("Index is not single-column");
        }
        return key_columns[0].max_varchar_len;
    }

    BTreeIndex *GetBTreeIndex() const {
        return runtime_index != nullptr ? runtime_index->AsBTreeIndex() : nullptr;
    }

    GenericBTreeIndex *GetGenericBTreeIndex() const {
        return dynamic_cast<GenericBTreeIndex *>(runtime_index);
    }
};

struct RelationCatalogEntry {
    RelationId relation_id{0};
    std::string relation_name;
    std::string heap_file_name;
    Schema schema;
    HeapFile *heap_file{nullptr};
    std::vector<IndexCatalogEntry> indexes;
};

class CatalogManager {
public:
    explicit CatalogManager(std::string catalog_file_path = "");

    void RegisterRelation(RelationId relation_id,
                          const std::string &relation_name,
                          const Schema &schema,
                          HeapFile *heap_file,
                          const std::string &heap_file_name = "");

    void RegisterIndex(const IndexCatalogEntry &index_entry);

    void RegisterIndex(const std::string &index_name,
                       RelationId base_relation_id,
                       RelationId index_relation_id,
                       std::size_t key_column_idx,
                       TypeId key_type,
                       uint32_t max_varchar_len,
                       BTreeIndex *index,
                       bool is_unique = false,
                       NullPolicy null_policy = NullPolicy::NOT_SUPPORTED,
                       const std::string &index_file_name = "");

    const RelationCatalogEntry &GetRelation(RelationId relation_id) const;
    RelationCatalogEntry &GetRelationMutable(RelationId relation_id);
    const RelationCatalogEntry &GetRelationByName(const std::string &relation_name) const;
    const std::unordered_map<RelationId, RelationCatalogEntry> &GetAllRelations() const { return relations_by_id_; }

    const IndexCatalogEntry *FindIndexOnColumn(RelationId relation_id,
                                               std::size_t column_idx) const;
    const std::vector<IndexCatalogEntry> &GetIndexes(RelationId relation_id) const;
    const IndexCatalogEntry *FindIndexByName(RelationId relation_id,
                                             const std::string &index_name) const;


    void AttachHeapFile(RelationId relation_id, HeapFile *heap_file);
    void AttachIndex(RelationId base_relation_id, const std::string &index_name, AbstractIndex *index);
    void AttachIndex(RelationId base_relation_id, const std::string &index_name, BTreeIndex *index);
    void AttachIndex(RelationId base_relation_id, const std::string &index_name, GenericBTreeIndex *index);

    bool Save() const;
    bool Load();
    const std::string &GetCatalogFilePath() const { return catalog_file_path_; }

private:
    std::unordered_map<RelationId, RelationCatalogEntry> relations_by_id_;
    std::unordered_map<std::string, RelationId> relation_name_to_id_;
    std::string catalog_file_path_;
};

}  // namespace simpledb
