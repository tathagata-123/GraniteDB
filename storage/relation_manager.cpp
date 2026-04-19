#include "relation_manager.h"

#include <stdexcept>

#include "slotted_page.h"

namespace simpledb {

RelationManager::RelationManager(DiskManager *disk_manager,
                                 BufferPoolManager *buffer_pool_manager)
    : next_relation_id_(1),
      disk_manager_(disk_manager),
      buffer_pool_manager_(buffer_pool_manager) {}

RelationId RelationManager::CreateHeapRelation(const std::string &relation_name,
                                               const Schema &schema,
                                               const std::string &heap_file_name) {
    std::lock_guard<std::mutex> guard(latch_);

    if (relation_name_to_id_.count(relation_name) != 0) {
        throw std::runtime_error("Relation name already exists: " + relation_name);
    }

    RelationId relation_id = next_relation_id_++;
    std::string file_name = heap_file_name.empty() ? (relation_name + ".heap") : heap_file_name;

    disk_manager_->CreateRelation(relation_id, file_name);

    RelationMetadata meta;
    meta.relation_id = relation_id;
    meta.relation_name = relation_name;
    meta.heap_file_name = file_name;
    meta.schema = schema;

    relations_by_id_[relation_id] = meta;
    relation_name_to_id_[relation_name] = relation_id;
    free_space_maps_[relation_id] = std::make_unique<FreeSpaceMap>();

    return relation_id;
}

void RelationManager::RegisterExistingHeapRelation(RelationId relation_id,
                                                   const std::string &relation_name,
                                                   const Schema &schema,
                                                   const std::string &heap_file_name) {
    std::lock_guard<std::mutex> guard(latch_);

    if (relations_by_id_.count(relation_id) != 0) {
        throw std::runtime_error("Relation id already registered");
    }
    if (relation_name_to_id_.count(relation_name) != 0) {
        throw std::runtime_error("Relation name already registered");
    }

    disk_manager_->OpenRelation(relation_id, heap_file_name);

    RelationMetadata meta;
    meta.relation_id = relation_id;
    meta.relation_name = relation_name;
    meta.heap_file_name = heap_file_name;
    meta.schema = schema;

    relations_by_id_[relation_id] = meta;
    relation_name_to_id_[relation_name] = relation_id;
    free_space_maps_[relation_id] = std::make_unique<FreeSpaceMap>();

    if (relation_id >= next_relation_id_) {
        next_relation_id_ = relation_id + 1;
    }
}

RelationId RelationManager::AllocateRelationId() {
    std::lock_guard<std::mutex> guard(latch_);
    return next_relation_id_++;
}

void RelationManager::EnsureNextRelationIdAtLeast(RelationId min_next_relation_id) {
    std::lock_guard<std::mutex> guard(latch_);
    if (next_relation_id_ < min_next_relation_id) {
        next_relation_id_ = min_next_relation_id;
    }
}

bool RelationManager::HasRelation(RelationId relation_id) const {
    std::lock_guard<std::mutex> guard(latch_);
    return relations_by_id_.count(relation_id) != 0;
}

RelationId RelationManager::GetRelationIdByName(const std::string &relation_name) const {
    std::lock_guard<std::mutex> guard(latch_);
    auto it = relation_name_to_id_.find(relation_name);
    if (it == relation_name_to_id_.end()) {
        throw std::runtime_error("Unknown relation name: " + relation_name);
    }
    return it->second;
}

const RelationMetadata &RelationManager::GetRelationMetadata(RelationId relation_id) const {
    std::lock_guard<std::mutex> guard(latch_);
    auto it = relations_by_id_.find(relation_id);
    if (it == relations_by_id_.end()) {
        throw std::runtime_error("Unknown relation id");
    }
    return it->second;
}

const RelationMetadata &RelationManager::GetRelationMetadata(const std::string &relation_name) const {
    return GetRelationMetadata(GetRelationIdByName(relation_name));
}

RelationMetadata &RelationManager::GetRelationMetadataMutable(RelationId relation_id) {
    auto it = relations_by_id_.find(relation_id);
    if (it == relations_by_id_.end()) {
        throw std::runtime_error("Unknown relation id");
    }
    return it->second;
}

FreeSpaceMap *RelationManager::GetFreeSpaceMap(RelationId relation_id) {
    std::lock_guard<std::mutex> guard(latch_);
    auto it = free_space_maps_.find(relation_id);
    if (it == free_space_maps_.end()) {
        throw std::runtime_error("No free-space map for relation");
    }
    return it->second.get();
}

const FreeSpaceMap *RelationManager::GetFreeSpaceMap(RelationId relation_id) const {
    std::lock_guard<std::mutex> guard(latch_);
    auto it = free_space_maps_.find(relation_id);
    if (it == free_space_maps_.end()) {
        throw std::runtime_error("No free-space map for relation");
    }
    return it->second.get();
}

void RelationManager::RegisterIndexFile(RelationId relation_id,
                                        const std::string &index_name,
                                        const std::string &index_file_name,
                                        PageId root_page_id) {
    std::lock_guard<std::mutex> guard(latch_);
    RelationMetadata &meta = GetRelationMetadataMutable(relation_id);
    meta.indexes.push_back(IndexMetadata{index_name, index_file_name, root_page_id});
}

void RelationManager::UpdateIndexRootPage(RelationId relation_id,
                                          const std::string &index_name,
                                          PageId root_page_id) {
    std::lock_guard<std::mutex> guard(latch_);
    RelationMetadata &meta = GetRelationMetadataMutable(relation_id);

    for (auto &idx : meta.indexes) {
        if (idx.index_name == index_name) {
            idx.root_page_id = root_page_id;
            return;
        }
    }

    throw std::runtime_error("Index not found: " + index_name);
}

void RelationManager::BuildFreeSpaceMap(RelationId relation_id) {
    FreeSpaceMap *fsm = GetFreeSpaceMap(relation_id);
    fsm->Clear();

    uint32_t num_pages = disk_manager_->GetNumPages(relation_id);
    const uint16_t empty_heap_page_insertable =
        static_cast<uint16_t>(PAGE_SIZE - sizeof(SlottedPageHeader) - sizeof(SlotEntry));

    for (PageNo page_no = 0; page_no < num_pages; page_no++) {
        PageId pid{relation_id, page_no};
        Page *page = buffer_pool_manager_->FetchPage(pid);
        if (page == nullptr) {
            throw std::runtime_error("Failed to fetch page while building free-space map");
        }

        page->RLatch();

        uint16_t free_bytes = 0;
        SlottedPage sp(page);

        if (!sp.IsInitialized()) {
            free_bytes = empty_heap_page_insertable;
        } else if (sp.GetPageType() == PageType::HEAP) {
            free_bytes = sp.GetMaxInsertableBytesAfterCompaction();
        } else {
            free_bytes = 0;
        }

        page->RUnlatch();
        buffer_pool_manager_->UnpinPage(pid, false);

        fsm->UpdatePageFreeSpace(page_no, free_bytes);
    }
}

}  // namespace simpledb
