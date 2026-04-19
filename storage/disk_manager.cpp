#include "disk_manager.h"

#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "../common/durable_io.h"

namespace simpledb {
namespace {

std::string SerializeFreeList(uint32_t num_pages, const std::set<PageNo> &free_pages) {
    std::ostringstream out;
    out << "FREELIST_V1\n";
    out << num_pages << "\n";
    out << free_pages.size() << "\n";
    for (PageNo page_no : free_pages) {
        out << page_no << "\n";
    }
    return out.str();
}

}  // namespace

DiskManager::DiskManager(const std::string &db_dir) : db_dir_(db_dir) {
    std::filesystem::create_directories(db_dir_);
    DurableSyncParentDirectory(db_dir_ + "/.sync_marker");
}

DiskManager::~DiskManager() { Shutdown(); }

std::string DiskManager::BuildPath(const std::string &file_name) const {
    return db_dir_ + "/" + file_name;
}

std::string DiskManager::BuildAllocatorStatePath(const std::string &file_name) const {
    return db_dir_ + "/" + file_name + ".freelist";
}

uint64_t DiskManager::FileOffset(PageNo page_no) const {
    return static_cast<uint64_t>(page_no) * PAGE_SIZE;
}

DiskManager::RelationHandle &DiskManager::GetHandle(RelationId relation_id) {
    auto it = relations_.find(relation_id);
    if (it == relations_.end()) {
        throw std::runtime_error("Relation is not open");
    }
    return it->second;
}

const DiskManager::RelationHandle &DiskManager::GetHandle(RelationId relation_id) const {
    auto it = relations_.find(relation_id);
    if (it == relations_.end()) {
        throw std::runtime_error("Relation is not open");
    }
    return it->second;
}

void DiskManager::ReopenHandleFile(RelationHandle *handle) const {
    handle->file.close();
    handle->file.clear();
    handle->file.open(handle->path, std::ios::binary | std::ios::in | std::ios::out);
    if (!handle->file.is_open()) {
        throw std::runtime_error("Failed to reopen relation file: " + handle->path);
    }
}

void DiskManager::ZeroPage(RelationHandle *handle, PageNo page_no) {
    std::vector<char> zero_page(PAGE_SIZE, 0);
    handle->file.clear();
    handle->file.seekp(FileOffset(page_no), std::ios::beg);
    handle->file.write(zero_page.data(), PAGE_SIZE);
    if (!handle->file.good()) {
        throw std::runtime_error("Failed to zero relation page");
    }
    handle->file.flush();
    DurableSyncPath(handle->path);
}

void DiskManager::TrimTrailingFreePages(RelationHandle *handle) {
    uint32_t old_num_pages = handle->num_pages;
    while (!handle->free_pages.empty()) {
        auto it = handle->free_pages.end();
        --it;
        if (*it + 1 != handle->num_pages) {
            break;
        }
        handle->free_pages.erase(it);
        handle->num_pages--;
    }

    if (handle->num_pages == old_num_pages) {
        return;
    }

    handle->file.flush();
    handle->file.clear();
    std::filesystem::resize_file(handle->path, static_cast<uint64_t>(handle->num_pages) * PAGE_SIZE);
    DurableSyncPath(handle->path);
    ReopenHandleFile(handle);
}

void DiskManager::LoadAllocatorState(RelationHandle *handle) {
    handle->free_pages.clear();

    std::ifstream in(handle->allocator_state_path);
    if (!in.is_open()) {
        return;
    }

    std::string magic;
    std::getline(in, magic);
    if (magic != "FREELIST_V1") {
        throw std::runtime_error("Unsupported allocator state format: " + handle->allocator_state_path);
    }

    std::string line;
    if (!std::getline(in, line)) {
        throw std::runtime_error("Corrupt allocator state: missing num_pages");
    }
    uint32_t persisted_num_pages = static_cast<uint32_t>(std::stoul(line));
    if (!std::getline(in, line)) {
        throw std::runtime_error("Corrupt allocator state: missing free-page count");
    }
    std::size_t free_count = static_cast<std::size_t>(std::stoull(line));

    if (persisted_num_pages > handle->num_pages) {
        throw std::runtime_error("Allocator state refers to more pages than exist on disk");
    }

    for (std::size_t i = 0; i < free_count; ++i) {
        if (!std::getline(in, line)) {
            throw std::runtime_error("Corrupt allocator state: truncated free-page list");
        }
        PageNo page_no = static_cast<PageNo>(std::stoul(line));
        if (page_no >= handle->num_pages) {
            throw std::runtime_error("Allocator state contains out-of-range free page");
        }
        handle->free_pages.insert(page_no);
    }
}

void DiskManager::PersistAllocatorState(const RelationHandle &handle) const {
    AtomicWriteStringFile(handle.allocator_state_path,
                          SerializeFreeList(handle.num_pages, handle.free_pages));
}

void DiskManager::CreateRelation(RelationId relation_id, const std::string &file_name) {
    std::string path = BuildPath(file_name);
    {
        std::ofstream create_file(path, std::ios::binary | std::ios::trunc);
        if (!create_file.is_open()) {
            throw std::runtime_error("Failed to create relation file: " + path);
        }
        create_file.flush();
    }
    DurableSyncPath(path);
    DurableSyncParentDirectory(path);

    AtomicWriteStringFile(BuildAllocatorStatePath(file_name), SerializeFreeList(0, {}));
    OpenRelation(relation_id, file_name);
}

void DiskManager::OpenRelation(RelationId relation_id, const std::string &file_name) {
    std::string path = BuildPath(file_name);

    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open relation file: " + path);
    }

    file.seekg(0, std::ios::end);
    std::streamoff file_size = file.tellg();
    if (file_size < 0) {
        throw std::runtime_error("Failed to determine file size");
    }

    RelationHandle handle;
    handle.path = path;
    handle.allocator_state_path = BuildAllocatorStatePath(file_name);
    handle.file = std::move(file);
    handle.num_pages = static_cast<uint32_t>(file_size / PAGE_SIZE);

    LoadAllocatorState(&handle);
    relations_[relation_id] = std::move(handle);
}

void DiskManager::CloseRelation(RelationId relation_id) {
    auto it = relations_.find(relation_id);
    if (it == relations_.end()) {
        return;
    }

    PersistAllocatorState(it->second);
    it->second.file.flush();
    DurableSyncPath(it->second.path);
    it->second.file.close();
    relations_.erase(it);
}

void DiskManager::DestroyRelation(RelationId relation_id) {
    auto it = relations_.find(relation_id);
    if (it == relations_.end()) {
        return;
    }

    std::string path = it->second.path;
    std::string allocator_state_path = it->second.allocator_state_path;
    it->second.file.flush();
    DurableSyncPath(path);
    it->second.file.close();
    relations_.erase(it);

    std::filesystem::remove(path);
    std::filesystem::remove(allocator_state_path);
    DurableSyncParentDirectory(path);
}

PageId DiskManager::AllocatePage(RelationId relation_id) {
    RelationHandle &handle = GetHandle(relation_id);

    if (!handle.free_pages.empty()) {
        auto it = handle.free_pages.begin();
        PageNo reused_page_no = *it;
        handle.free_pages.erase(it);
        ZeroPage(&handle, reused_page_no);
        PersistAllocatorState(handle);
        return PageId{relation_id, reused_page_no};
    }

    PageNo new_page_no = handle.num_pages;
    std::vector<char> zero_page(PAGE_SIZE, 0);

    handle.file.clear();
    handle.file.seekp(FileOffset(new_page_no), std::ios::beg);
    handle.file.write(zero_page.data(), PAGE_SIZE);
    if (!handle.file.good()) {
        throw std::runtime_error("Failed to allocate page");
    }

    handle.file.flush();
    handle.num_pages++;
    DurableSyncPath(handle.path);
    PersistAllocatorState(handle);

    return PageId{relation_id, new_page_no};
}

void DiskManager::DeallocatePage(PageId page_id) {
    RelationHandle &handle = GetHandle(page_id.relation_id);
    if (page_id.page_no >= handle.num_pages) {
        throw std::runtime_error("DeallocatePage: page does not exist");
    }
    handle.free_pages.insert(page_id.page_no);
    TrimTrailingFreePages(&handle);
    PersistAllocatorState(handle);
}

void DiskManager::ReadPage(PageId page_id, char *out_data) {
    RelationHandle &handle = GetHandle(page_id.relation_id);

    if (page_id.page_no >= handle.num_pages) {
        throw std::runtime_error("ReadPage: page does not exist");
    }

    handle.file.clear();
    handle.file.seekg(FileOffset(page_id.page_no), std::ios::beg);
    handle.file.read(out_data, PAGE_SIZE);

    if (handle.file.gcount() != static_cast<std::streamsize>(PAGE_SIZE)) {
        throw std::runtime_error("ReadPage: failed to read full page");
    }
}

void DiskManager::WritePage(PageId page_id, const char *in_data) {
    RelationHandle &handle = GetHandle(page_id.relation_id);

    if (page_id.page_no >= handle.num_pages) {
        throw std::runtime_error("WritePage: page does not exist");
    }

    handle.file.clear();
    handle.file.seekp(FileOffset(page_id.page_no), std::ios::beg);
    handle.file.write(in_data, PAGE_SIZE);
    if (!handle.file.good()) {
        throw std::runtime_error("WritePage: failed to write full page");
    }
}

uint32_t DiskManager::GetNumPages(RelationId relation_id) const {
    const RelationHandle &handle = GetHandle(relation_id);
    return handle.num_pages;
}

void DiskManager::Sync(RelationId relation_id) {
    RelationHandle &handle = GetHandle(relation_id);
    handle.file.flush();
    if (!handle.file.good()) {
        throw std::runtime_error("Failed to flush relation file");
    }
    DurableSyncPath(handle.path);
    PersistAllocatorState(handle);
}

void DiskManager::Shutdown() {
    for (auto &entry : relations_) {
        PersistAllocatorState(entry.second);
        entry.second.file.flush();
        DurableSyncPath(entry.second.path);
        entry.second.file.close();
    }
    relations_.clear();
}

}  // namespace simpledb
