// heap file: table rows in a chain of slotted pages.
// slot array grows down from the header, records grow up from the end:
//   [ header | slot0 slot1 -> ... <- rec1 rec0 ]
// slot len==0 = tombstone (deleted), so RecordId(page,slot) stays stable.
#pragma once

#include "common.h"
#include "buffer_pool.h"
#include "tuple.h"
#include <cstring>
#include <optional>

using namespace std;

namespace minidb {

#pragma pack(push, 1)
struct PageHeader {
    PageId next_page = INVALID_PAGE_ID;  // next page in chain
    int16_t num_slots = 0;               // includes tombstones
    int16_t free_ptr = PAGE_SIZE;        // lowest record byte
};
struct Slot {
    int16_t offset = 0;
    int16_t length = 0;  // 0 = deleted
};
#pragma pack(pop)

constexpr int SLOT_ARRAY_START = sizeof(PageHeader);

// wraps a buffer pool frame and reads it as a slotted page
class HeapPage {
public:
    explicit HeapPage(Page* page) : page_(page) {}

    PageHeader* header() { return reinterpret_cast<PageHeader*>(page_->data); }
    Slot* slots() { return reinterpret_cast<Slot*>(page_->data + SLOT_ARRAY_START); }

    void init() {
        auto* h = header();
        h->next_page = INVALID_PAGE_ID;
        h->num_slots = 0;
        h->free_ptr = PAGE_SIZE;
    }

    // free bytes, minus room for the new slot
    int free_space() {
        auto* h = header();
        int slot_end = SLOT_ARRAY_START + h->num_slots * static_cast<int>(sizeof(Slot));
        return h->free_ptr - slot_end;
    }

    // returns slot idx, or -1 if it won't fit
    int insert(const string& rec) {
        auto* h = header();
        int need = static_cast<int>(rec.size()) + static_cast<int>(sizeof(Slot));
        if (need > free_space()) return -1;
        int16_t new_off = h->free_ptr - static_cast<int16_t>(rec.size());
        memcpy(page_->data + new_off, rec.data(), rec.size());
        h->free_ptr = new_off;
        int idx = h->num_slots++;
        slots()[idx] = Slot{new_off, static_cast<int16_t>(rec.size())};
        return idx;
    }

    // nullopt if out of range or tombstoned
    optional<string> get(int idx) {
        auto* h = header();
        if (idx < 0 || idx >= h->num_slots) return nullopt;
        Slot s = slots()[idx];
        if (s.length == 0) return nullopt;
        return string(page_->data + s.offset, page_->data + s.offset + s.length);
    }

    // just tombstone it, no compaction. keeps the slot id valid for indexes
    bool erase(int idx) {
        auto* h = header();
        if (idx < 0 || idx >= h->num_slots) return false;
        if (slots()[idx].length == 0) return false;
        slots()[idx].length = 0;
        return true;
    }

    int16_t num_slots() { return header()->num_slots; }

private:
    Page* page_;
};

// one heap file = all the pages for a table, chained via next_page
class HeapFile {
public:
    HeapFile(BufferPool* bp, PageId first_page) : bp_(bp), first_page_(first_page) {}

    static PageId create(BufferPool* bp) {
        PageId pid;
        Page* p = bp->new_page(&pid);
        HeapPage hp(p);
        hp.init();
        bp->unpin_page(pid, true);
        return pid;
    }

    PageId first_page() const { return first_page_; }

    // walk the chain till we find room, make a new page if none.
    // start from last_page_ hint not the head, otherwise filling N rows is O(N^2).
    // works because we never compact, so free space is always at the tail.
    RecordId insert(const string& rec) {
        PageId pid = (last_page_ != INVALID_PAGE_ID) ? last_page_ : first_page_;
        while (true) {
            Page* p = bp_->fetch_page(pid);
            HeapPage hp(p);
            int slot = hp.insert(rec);
            if (slot >= 0) {
                last_page_ = pid;
                bp_->unpin_page(pid, true);
                return RecordId{pid, static_cast<int16_t>(slot)};
            }
            PageId next = hp.header()->next_page;
            if (next == INVALID_PAGE_ID) {
                // end of chain, add a new page
                PageId new_pid;
                Page* np = bp_->new_page(&new_pid);
                HeapPage nhp(np);
                nhp.init();
                int s = nhp.insert(rec);
                bp_->unpin_page(new_pid, true);
                hp.header()->next_page = new_pid;
                bp_->unpin_page(pid, true);
                last_page_ = new_pid;
                return RecordId{new_pid, static_cast<int16_t>(s)};
            }
            bp_->unpin_page(pid, false);
            pid = next;
        }
    }

    optional<string> get(RecordId rid) {
        Page* p = bp_->fetch_page(rid.page_id);
        HeapPage hp(p);
        auto r = hp.get(rid.slot);
        bp_->unpin_page(rid.page_id, false);
        return r;
    }

    bool erase(RecordId rid) {
        Page* p = bp_->fetch_page(rid.page_id);
        HeapPage hp(p);
        bool ok = hp.erase(rid.slot);
        bp_->unpin_page(rid.page_id, ok);
        return ok;
    }

    // calls fn(rid, bytes) on every live record. return false from fn to stop.
    template <typename Fn>
    void scan(Fn&& fn) {
        PageId pid = first_page_;
        while (pid != INVALID_PAGE_ID) {
            Page* p = bp_->fetch_page(pid);
            HeapPage hp(p);
            int16_t cnt = hp.num_slots();
            PageId next = hp.header()->next_page;
            for (int16_t i = 0; i < cnt; ++i) {
                auto rec = hp.get(i);
                if (rec) {
                    if (!fn(RecordId{pid, i}, *rec)) {
                        bp_->unpin_page(pid, false);
                        return;
                    }
                }
            }
            bp_->unpin_page(pid, false);
            pid = next;
        }
    }

private:
    BufferPool* bp_;
    PageId first_page_;
    PageId last_page_ = INVALID_PAGE_ID;  // tail hint for fast appends
};

}  // namespace minidb
