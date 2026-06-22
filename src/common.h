// common types/constants used everywhere
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

using namespace std;

namespace minidb {

constexpr int PAGE_SIZE = 4096;   // page = unit of disk I/O

using PageId = int32_t;
constexpr PageId INVALID_PAGE_ID = -1;

// physical address of a record: page + slot in that page
struct RecordId {
    PageId page_id = INVALID_PAGE_ID;
    int16_t slot = -1;

    bool operator==(const RecordId& o) const {
        return page_id == o.page_id && slot == o.slot;
    }
    bool valid() const { return page_id != INVALID_PAGE_ID && slot >= 0; }
};

using TxnId = uint64_t;
constexpr TxnId INVALID_TXN_ID = 0;   // 0 = no txn

using Lsn = uint64_t;
constexpr Lsn INVALID_LSN = 0;

struct DBError : runtime_error {
    explicit DBError(const string& msg) : runtime_error(msg) {}
};

}  // namespace minidb
