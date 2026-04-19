#pragma once

#include <cstdint>
#include <fstream>
#include <set>
#include <string>
#include <unordered_map>

#include "../common/types.h"

namespace simpledb {

class DiskManager {
public:
    explicit DiskManager(const std::string &db_dir);
    ~DiskManager();

    void CreateRelation(RelationId relation_id, const std::string &file_name);
    void OpenRelation(RelationId relation_id, const std::string &file_name);
    void CloseRelation(RelationId relation_id);
    void DestroyRelation(RelationId relation_id);

    PageId AllocatePage(RelationId relation_id);
    void DeallocatePage(PageId page_id);

    void ReadPage(PageId page_id, char *out_data);
    void WritePage(PageId page_id, const char *in_data);

    uint32_t GetNumPages(RelationId relation_id) const;

    void Sync(RelationId relation_id);
    void Shutdown();

private:
    struct RelationHandle {
        std::string path;
        std::string allocator_state_path;
        mutable std::fstream file;
        uint32_t num_pages{0};
        std::set<PageNo> free_pages;
    };

    std::string db_dir_;
    std::unordered_map<RelationId, RelationHandle> relations_;

    std::string BuildPath(const std::string &file_name) const;
    std::string BuildAllocatorStatePath(const std::string &file_name) const;
    RelationHandle &GetHandle(RelationId relation_id);
    const RelationHandle &GetHandle(RelationId relation_id) const;
    uint64_t FileOffset(PageNo page_no) const;

    void ZeroPage(RelationHandle *handle, PageNo page_no);
    void ReopenHandleFile(RelationHandle *handle) const;
    void TrimTrailingFreePages(RelationHandle *handle);
    void LoadAllocatorState(RelationHandle *handle);
    void PersistAllocatorState(const RelationHandle &handle) const;
};

}  // namespace simpledb
