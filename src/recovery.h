// crash recovery on top of the wal. runs at startup before any query.
// records are physical per-slot images so redo/undo are just slot writes.
#pragma once

#include "wal.h"
#include "buffer_pool.h"
#include "heap_file.h"
#include <unordered_set>
#include <unordered_map>

using namespace std;

namespace minidb {

class Recovery {
public:
    Recovery(WAL* wal, BufferPool* bp) : wal_(wal), bp_(bp) {}

    struct Report { int committed = 0; int rolled_back = 0; int redone = 0; int undone = 0; };

    // 3 passes: find committed txns, redo everything, undo the losers.
    Report recover() {
        Report rep;
        vector<LogRecord> log = wal_->read_all();

        // analysis: who committed
        unordered_set<TxnId> committed, started;
        for (auto& r : log) {
            if (r.type == LogType::BEGIN) started.insert(r.txn);
            else if (r.type == LogType::COMMIT) committed.insert(r.txn);
        }
        rep.committed = (int)committed.size();

        // redo all data changes in order, rebuilds page state at crash time
        for (auto& r : log) {
            if (!is_data_op(r.type)) continue;
            redo(r);
            rep.redone++;
        }

        // undo losers (started but never committed) in reverse
        unordered_set<TxnId> losers;
        for (TxnId t : started) if (!committed.count(t)) losers.insert(t);
        rep.rolled_back = (int)losers.size();
        for (auto it = log.rbegin(); it != log.rend(); ++it) {
            if (!is_data_op(it->type)) continue;
            if (!losers.count(it->txn)) continue;
            undo(*it);
            rep.undone++;
        }

        bp_->flush_all();
        return rep;
    }

private:
    static bool is_data_op(LogType t) {
        return t == LogType::INSERT || t == LogType::DELETE || t == LogType::UPDATE;
    }

    void write_slot(PageId pid, int16_t slot, const string& image) {
        Page* p = bp_->fetch_page(pid);
        HeapPage hp(p);
        auto* h = hp.header();
        if (slot < h->num_slots && image.size() == (size_t)hp.slots()[slot].length) {
            // same length, overwrite in place
            memcpy(p->data + hp.slots()[slot].offset, image.data(), image.size());
        } else {
            // slot not there yet (e.g. insert redo that never hit disk), rebuild it
            ensure_slot(hp, slot, image);
        }
        bp_->unpin_page(pid, true);
    }

    void tombstone(PageId pid, int16_t slot) {
        Page* p = bp_->fetch_page(pid);
        HeapPage hp(p);
        if (slot < hp.header()->num_slots) hp.slots()[slot].length = 0;
        bp_->unpin_page(pid, true);
    }

    // make slot hold image, grow slot array if needed
    void ensure_slot(HeapPage& hp, int16_t slot, const string& image) {
        auto* h = hp.header();
        while (h->num_slots <= slot) {  // fill gaps with empty slots
            int idx = h->num_slots++;
            hp.slots()[idx] = Slot{0, 0};
        }
        int16_t off = h->free_ptr - (int16_t)image.size();
        memcpy(reinterpret_cast<char*>(h) + off, image.data(), image.size());
        h->free_ptr = off;
        hp.slots()[slot] = Slot{off, (int16_t)image.size()};
    }

    void redo(const LogRecord& r) {
        switch (r.type) {
            case LogType::INSERT: write_slot(r.page, r.slot, r.after); break;
            case LogType::UPDATE: write_slot(r.page, r.slot, r.after); break;
            case LogType::DELETE: tombstone(r.page, r.slot); break;
            default: break;
        }
    }
    void undo(const LogRecord& r) {
        switch (r.type) {
            case LogType::INSERT: tombstone(r.page, r.slot); break;
            case LogType::UPDATE: write_slot(r.page, r.slot, r.before); break;
            case LogType::DELETE: write_slot(r.page, r.slot, r.before); break;
            default: break;
        }
    }

    WAL* wal_;
    BufferPool* bp_;
};

}  // namespace minidb
