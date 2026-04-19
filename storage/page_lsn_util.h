#pragma once

#include <cstring>

#include "../common/types.h"
#include "page.h"

namespace simpledb {

inline LSN GetPageLSN(const char *page_data) {
    LSN lsn = 0;
    std::memcpy(&lsn, page_data, sizeof(lsn));
    return lsn;
}

inline void SetPageLSN(char *page_data, LSN lsn) {
    std::memcpy(page_data, &lsn, sizeof(lsn));
}

inline LSN GetPageLSN(const Page *page) {
    return GetPageLSN(page->GetData());
}

inline void SetPageLSN(Page *page, LSN lsn) {
    SetPageLSN(page->GetData(), lsn);
}

}  // namespace simpledb
