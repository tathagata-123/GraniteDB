#include "catalog_manager.h"

#include "../access/btree.h"
#include "../access/generic_btree.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "../common/durable_io.h"

namespace simpledb {
namespace {

std::string Escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '|') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::string Unescape(const std::string &s) {
    std::string out;
    bool esc = false;
    for (char c : s) {
        if (esc) {
            out.push_back(c);
            esc = false;
        } else if (c == '\\') {
            esc = true;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::vector<std::string> SplitEscaped(const std::string &line) {
    std::vector<std::string> parts;
    std::string cur;
    bool esc = false;
    for (char c : line) {
        if (esc) {
            cur.push_back(c);
            esc = false;
            continue;
        }
        if (c == '\\') {
            esc = true;
            continue;
        }
        if (c == '|') {
            parts.push_back(cur);
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    parts.push_back(cur);
    return parts;
}

IndexCatalogEntry ParseV1Index(const std::vector<std::string> &parts) {
    if (parts.size() != 8 || parts[0] != "IDX") {
        throw std::runtime_error("Corrupt V1 catalog index entry");
    }
    IndexCatalogEntry idx;
    idx.index_name = Unescape(parts[1]);
    idx.base_relation_id = static_cast<RelationId>(std::stoul(parts[2]));
    idx.index_relation_id = static_cast<RelationId>(std::stoul(parts[3]));
    idx.key_columns.push_back(IndexKeyColumnDefinition{
        static_cast<std::size_t>(std::stoull(parts[4])),
        static_cast<TypeId>(std::stoi(parts[5])),
        static_cast<uint32_t>(std::stoul(parts[6])),
        true
    });
    idx.root_page_no = static_cast<PageNo>(std::stoul(parts[7]));
    idx.is_unique = false;
    idx.null_policy = NullPolicy::NOT_SUPPORTED;
    idx.kind = IndexKind::BTREE;
    idx.index_file_name = idx.index_name + ".idx";
    return idx;
}

}  // namespace

CatalogManager::CatalogManager(std::string catalog_file_path)
    : catalog_file_path_(std::move(catalog_file_path)) {}

void CatalogManager::RegisterRelation(RelationId relation_id,
                                      const std::string &relation_name,
                                      const Schema &schema,
                                      HeapFile *heap_file,
                                      const std::string &heap_file_name) {
    if (relations_by_id_.count(relation_id) != 0) {
        throw std::runtime_error("Relation id already registered in catalog");
    }
    if (relation_name_to_id_.count(relation_name) != 0) {
        throw std::runtime_error("Relation name already registered in catalog");
    }
    RelationCatalogEntry entry;
    entry.relation_id = relation_id;
    entry.relation_name = relation_name;
    entry.heap_file_name = heap_file_name.empty() ? (relation_name + ".heap") : heap_file_name;
    entry.schema = schema;
    entry.heap_file = heap_file;
    relations_by_id_[relation_id] = entry;
    relation_name_to_id_[relation_name] = relation_id;
    Save();
}

void CatalogManager::RegisterIndex(const IndexCatalogEntry &index_entry) {
    auto it = relations_by_id_.find(index_entry.base_relation_id);
    if (it == relations_by_id_.end()) {
        throw std::runtime_error("Base relation not registered before index");
    }
    for (const auto &idx : it->second.indexes) {
        if (idx.index_name == index_entry.index_name) {
            throw std::runtime_error("Index name already registered on relation: " + index_entry.index_name);
        }
    }
    it->second.indexes.push_back(index_entry);
    Save();
}

void CatalogManager::RegisterIndex(const std::string &index_name,
                                   RelationId base_relation_id,
                                   RelationId index_relation_id,
                                   std::size_t key_column_idx,
                                   TypeId key_type,
                                   uint32_t max_varchar_len,
                                   BTreeIndex *index,
                                   bool is_unique,
                                   NullPolicy null_policy,
                                   const std::string &index_file_name) {
    IndexCatalogEntry idx;
    idx.index_name = index_name;
    idx.base_relation_id = base_relation_id;
    idx.index_relation_id = index_relation_id;
    idx.key_columns.push_back(IndexKeyColumnDefinition{key_column_idx, key_type, max_varchar_len, true});
    idx.is_unique = is_unique;
    idx.null_policy = null_policy;
    idx.kind = IndexKind::BTREE;
    idx.index_file_name = index_file_name.empty() ? (index_name + ".idx") : index_file_name;
    if (index != nullptr) {
        idx.root_page_no = index->GetRootPageNo();
        idx.runtime_index = new BTreeIndexAdapter(index);
    }
    RegisterIndex(idx);
}

const RelationCatalogEntry &CatalogManager::GetRelation(RelationId relation_id) const {
    auto it = relations_by_id_.find(relation_id);
    if (it == relations_by_id_.end()) {
        throw std::runtime_error("Unknown relation id in catalog");
    }
    return it->second;
}

RelationCatalogEntry &CatalogManager::GetRelationMutable(RelationId relation_id) {
    auto it = relations_by_id_.find(relation_id);
    if (it == relations_by_id_.end()) {
        throw std::runtime_error("Unknown relation id in catalog");
    }
    return it->second;
}

const RelationCatalogEntry &CatalogManager::GetRelationByName(const std::string &relation_name) const {
    auto it = relation_name_to_id_.find(relation_name);
    if (it == relation_name_to_id_.end()) {
        throw std::runtime_error("Unknown relation name in catalog");
    }
    return GetRelation(it->second);
}

const IndexCatalogEntry *CatalogManager::FindIndexOnColumn(RelationId relation_id,
                                                           std::size_t column_idx) const {
    const auto &rel = GetRelation(relation_id);
    for (const auto &idx : rel.indexes) {
        if (idx.MatchesSingleColumn(column_idx)) return &idx;
    }
    return nullptr;
}

const IndexCatalogEntry *CatalogManager::FindIndexByName(RelationId relation_id,
                                                         const std::string &index_name) const {
    const auto &rel = GetRelation(relation_id);
    for (const auto &idx : rel.indexes) {
        if (idx.index_name == index_name) return &idx;
    }
    return nullptr;
}

const std::vector<IndexCatalogEntry> &CatalogManager::GetIndexes(RelationId relation_id) const {
    return GetRelation(relation_id).indexes;
}

void CatalogManager::AttachHeapFile(RelationId relation_id, HeapFile *heap_file) {
    relations_by_id_.at(relation_id).heap_file = heap_file;
}

void CatalogManager::AttachIndex(RelationId base_relation_id,
                                 const std::string &index_name,
                                 AbstractIndex *index) {
    auto &indexes = relations_by_id_.at(base_relation_id).indexes;
    for (auto &idx : indexes) {
        if (idx.index_name == index_name) {
            idx.runtime_index = index;
            idx.root_page_no = index != nullptr ? index->GetRootPageNo() : idx.root_page_no;
            return;
        }
    }
    throw std::runtime_error("Index not found in catalog: " + index_name);
}

void CatalogManager::AttachIndex(RelationId base_relation_id,
                                 const std::string &index_name,
                                 BTreeIndex *index) {
    if (index == nullptr) {
        AttachIndex(base_relation_id, index_name, static_cast<AbstractIndex *>(nullptr));
        return;
    }
    AttachIndex(base_relation_id, index_name, new BTreeIndexAdapter(index));
}

void CatalogManager::AttachIndex(RelationId base_relation_id,
                                 const std::string &index_name,
                                 GenericBTreeIndex *index) {
    AttachIndex(base_relation_id, index_name, static_cast<AbstractIndex *>(index));
}

bool CatalogManager::Save() const {
    if (catalog_file_path_.empty()) return false;

    std::ostringstream out;
    out << "CATALOG_V3\n";
    out << relations_by_id_.size() << "\n";
    for (const auto &[rid, rel] : relations_by_id_) {
        out << "REL|" << rid << "|" << Escape(rel.relation_name) << "|" << Escape(rel.heap_file_name) << "|" << rel.schema.GetColumnCount() << "\n";
        for (const auto &col : rel.schema.GetColumns()) {
            out << "COL|" << Escape(col.GetName()) << "|" << static_cast<int>(col.GetType()) << "|"
                << (col.IsNullable() ? 1 : 0) << "|" << col.GetMaxLength() << "\n";
        }
        out << "IDXCOUNT|" << rel.indexes.size() << "\n";
        for (const auto &idx : rel.indexes) {
            out << "IDX|" << Escape(idx.index_name) << "|" << Escape(idx.index_file_name) << "|"
                << idx.base_relation_id << "|" << idx.index_relation_id << "|"
                << static_cast<int>(idx.kind) << "|" << (idx.is_unique ? 1 : 0) << "|"
                << static_cast<int>(idx.null_policy) << "|" << idx.GetRootPageNo() << "|" << idx.key_columns.size() << "\n";
            for (const auto &key_col : idx.key_columns) {
                out << "KEY|" << key_col.column_idx << "|" << static_cast<int>(key_col.type) << "|"
                    << key_col.max_varchar_len << "|" << (key_col.nullable ? 1 : 0) << "\n";
            }
        }
    }

    AtomicWriteStringFile(catalog_file_path_, out.str());
    return true;
}

bool CatalogManager::Load() {
    if (catalog_file_path_.empty()) return false;
    std::ifstream in(catalog_file_path_);
    if (!in.is_open()) return false;

    std::string magic;
    std::getline(in, magic);
    if (magic != "CATALOG_V1" && magic != "CATALOG_V2" && magic != "CATALOG_V3") {
        throw std::runtime_error("Unsupported catalog file format");
    }

    std::string line;
    std::getline(in, line);
    size_t rel_count = static_cast<size_t>(std::stoull(line));

    relations_by_id_.clear();
    relation_name_to_id_.clear();

    for (size_t r = 0; r < rel_count; ++r) {
        std::getline(in, line);
        auto rel_parts = SplitEscaped(line);
        RelationCatalogEntry rel;
        size_t col_count = 0;

        if (magic == "CATALOG_V3") {
            if (rel_parts.size() != 5 || rel_parts[0] != "REL") {
                throw std::runtime_error("Corrupt catalog relation entry");
            }
            rel.relation_id = static_cast<RelationId>(std::stoul(rel_parts[1]));
            rel.relation_name = Unescape(rel_parts[2]);
            rel.heap_file_name = Unescape(rel_parts[3]);
            col_count = static_cast<size_t>(std::stoull(rel_parts[4]));
        } else {
            if (rel_parts.size() != 4 || rel_parts[0] != "REL") {
                throw std::runtime_error("Corrupt catalog relation entry");
            }
            rel.relation_id = static_cast<RelationId>(std::stoul(rel_parts[1]));
            rel.relation_name = Unescape(rel_parts[2]);
            rel.heap_file_name = rel.relation_name + ".heap";
            col_count = static_cast<size_t>(std::stoull(rel_parts[3]));
        }

        std::vector<Column> cols;
        cols.reserve(col_count);
        for (size_t c = 0; c < col_count; ++c) {
            std::getline(in, line);
            auto col_parts = SplitEscaped(line);
            if (col_parts.size() != 5 || col_parts[0] != "COL") {
                throw std::runtime_error("Corrupt catalog column entry");
            }
            cols.emplace_back(Unescape(col_parts[1]),
                              static_cast<TypeId>(std::stoi(col_parts[2])),
                              std::stoi(col_parts[3]) != 0,
                              static_cast<uint32_t>(std::stoul(col_parts[4])));
        }
        rel.schema = Schema(std::move(cols));

        std::getline(in, line);
        auto idx_count_parts = SplitEscaped(line);
        if (idx_count_parts.size() != 2 || idx_count_parts[0] != "IDXCOUNT") {
            throw std::runtime_error("Corrupt catalog index-count entry");
        }
        size_t idx_count = static_cast<size_t>(std::stoull(idx_count_parts[1]));
        for (size_t i = 0; i < idx_count; ++i) {
            std::getline(in, line);
            auto idx_parts = SplitEscaped(line);
            IndexCatalogEntry idx;
            if (magic == "CATALOG_V1") {
                idx = ParseV1Index(idx_parts);
            } else if (magic == "CATALOG_V2") {
                if (idx_parts.size() != 9 || idx_parts[0] != "IDX") {
                    throw std::runtime_error("Corrupt catalog index entry");
                }
                idx.index_name = Unescape(idx_parts[1]);
                idx.index_file_name = idx.index_name + ".idx";
                idx.base_relation_id = static_cast<RelationId>(std::stoul(idx_parts[2]));
                idx.index_relation_id = static_cast<RelationId>(std::stoul(idx_parts[3]));
                idx.kind = static_cast<IndexKind>(std::stoi(idx_parts[4]));
                idx.is_unique = std::stoi(idx_parts[5]) != 0;
                idx.null_policy = static_cast<NullPolicy>(std::stoi(idx_parts[6]));
                idx.root_page_no = static_cast<PageNo>(std::stoul(idx_parts[7]));
                size_t key_count = static_cast<size_t>(std::stoull(idx_parts[8]));
                idx.key_columns.reserve(key_count);
                for (size_t k = 0; k < key_count; ++k) {
                    std::getline(in, line);
                    auto key_parts = SplitEscaped(line);
                    if (key_parts.size() != 5 || key_parts[0] != "KEY") {
                        throw std::runtime_error("Corrupt catalog index-key entry");
                    }
                    idx.key_columns.push_back(IndexKeyColumnDefinition{
                        static_cast<std::size_t>(std::stoull(key_parts[1])),
                        static_cast<TypeId>(std::stoi(key_parts[2])),
                        static_cast<uint32_t>(std::stoul(key_parts[3])),
                        std::stoi(key_parts[4]) != 0
                    });
                }
            } else {
                if (idx_parts.size() != 10 || idx_parts[0] != "IDX") {
                    throw std::runtime_error("Corrupt catalog index entry");
                }
                idx.index_name = Unescape(idx_parts[1]);
                idx.index_file_name = Unescape(idx_parts[2]);
                idx.base_relation_id = static_cast<RelationId>(std::stoul(idx_parts[3]));
                idx.index_relation_id = static_cast<RelationId>(std::stoul(idx_parts[4]));
                idx.kind = static_cast<IndexKind>(std::stoi(idx_parts[5]));
                idx.is_unique = std::stoi(idx_parts[6]) != 0;
                idx.null_policy = static_cast<NullPolicy>(std::stoi(idx_parts[7]));
                idx.root_page_no = static_cast<PageNo>(std::stoul(idx_parts[8]));
                size_t key_count = static_cast<size_t>(std::stoull(idx_parts[9]));
                idx.key_columns.reserve(key_count);
                for (size_t k = 0; k < key_count; ++k) {
                    std::getline(in, line);
                    auto key_parts = SplitEscaped(line);
                    if (key_parts.size() != 5 || key_parts[0] != "KEY") {
                        throw std::runtime_error("Corrupt catalog index-key entry");
                    }
                    idx.key_columns.push_back(IndexKeyColumnDefinition{
                        static_cast<std::size_t>(std::stoull(key_parts[1])),
                        static_cast<TypeId>(std::stoi(key_parts[2])),
                        static_cast<uint32_t>(std::stoul(key_parts[3])),
                        std::stoi(key_parts[4]) != 0
                    });
                }
            }
            rel.indexes.push_back(idx);
        }

        relations_by_id_[rel.relation_id] = rel;
        relation_name_to_id_[rel.relation_name] = rel.relation_id;
    }

    return true;
}

}  // namespace simpledb
