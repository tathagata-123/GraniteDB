#pragma once

#include <string>

namespace simpledb {

void DurableSyncPath(const std::string &path);
void DurableSyncParentDirectory(const std::string &path);
void AtomicWriteStringFile(const std::string &path, const std::string &contents);

}  // namespace simpledb
