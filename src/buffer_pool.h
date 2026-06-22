// buffer pool: caches pages in memory, evicts LRU when full.
// pinned pages never get evicted. dirty bit tracks if we need to write back.
#pragma once

#include "common.h"
#include "disk_manager.h"
#include <list>
#include <unordered_map>
#include <vector>
#include <mutex>

using namespace std;

namespace minidb {

// a frame = one page's data + bookkeeping
struct Page {
    PageId page_id = INVALID_PAGE_ID;
    int pin_count = 0;
    bool is_dirty = false;
    char data[PAGE_SIZE] = {0};

    void reset() {
        page_id = INVALID_PAGE_ID;
        pin_count = 0;
        is_dirty = false;
        fill(data, data + PAGE_SIZE, 0);
    }
};

class BufferPool {
public:
    BufferPool(DiskManager* disk, size_t pool_size)
        : disk_(disk), pool_size_(pool_size), frames_(pool_size) {
        for (size_t i = 0; i < pool_size_; ++i) free_list_.push_back(i);
    }

    // pins the page, caller has to unpin when done!
    Page* fetch_page(PageId pid) {
        lock_guard<mutex> lock(latch_);
        auto it = page_table_.find(pid);
        if (it != page_table_.end()) {
            size_t fid = it->second;
            frames_[fid].pin_count++;
            touch_lru(fid);
            return &frames_[fid];
        }
        // miss -> grab a frame and read from disk
        size_t fid;
        if (!grab_frame(fid)) throw DBError("BufferPool: all frames pinned, cannot fetch page");
        Page& f = frames_[fid];
        f.reset();
        f.page_id = pid;
        f.pin_count = 1;
        disk_->read_page(pid, f.data);
        page_table_[pid] = fid;
        touch_lru(fid);
        return &f;
    }

    Page* new_page(PageId* out_pid) {
        PageId pid = disk_->allocate_page();
        if (out_pid) *out_pid = pid;
        return fetch_page(pid);
    }

    // pass dirty=true if you modified the page
    void unpin_page(PageId pid, bool dirty) {
        lock_guard<mutex> lock(latch_);
        auto it = page_table_.find(pid);
        if (it == page_table_.end()) return;
        Page& f = frames_[it->second];
        if (dirty) f.is_dirty = true;
        if (f.pin_count > 0) f.pin_count--;
    }

    void flush_page(PageId pid) {
        lock_guard<mutex> lock(latch_);
        auto it = page_table_.find(pid);
        if (it == page_table_.end()) return;
        Page& f = frames_[it->second];
        if (f.is_dirty) {
            disk_->write_page(pid, f.data);
            f.is_dirty = false;
        }
    }

    // flush everything + fsync. call on shutdown/checkpoint
    void flush_all() {
        lock_guard<mutex> lock(latch_);
        for (auto& [pid, fid] : page_table_) {
            if (frames_[fid].is_dirty) {
                disk_->write_page(pid, frames_[fid].data);
                frames_[fid].is_dirty = false;
            }
        }
        disk_->sync();
    }

    DiskManager* disk() { return disk_; }

private:
    // free list first, otherwise evict. false if everything's pinned
    bool grab_frame(size_t& out_fid) {
        if (!free_list_.empty()) {
            out_fid = free_list_.front();
            free_list_.pop_front();
            return true;
        }
        // scan from the back (LRU end), skip pinned frames - can't touch those
        for (auto rit = lru_list_.rbegin(); rit != lru_list_.rend(); ++rit) {
            size_t fid = *rit;
            if (frames_[fid].pin_count == 0) {
                Page& victim = frames_[fid];
                if (victim.is_dirty) disk_->write_page(victim.page_id, victim.data);
                page_table_.erase(victim.page_id);
                lru_list_.erase(next(rit).base());
                lru_pos_.erase(fid);
                out_fid = fid;
                return true;
            }
        }
        return false;
    }

    // bump to front = most recently used
    void touch_lru(size_t fid) {
        auto it = lru_pos_.find(fid);
        if (it != lru_pos_.end()) lru_list_.erase(it->second);
        lru_list_.push_front(fid);
        lru_pos_[fid] = lru_list_.begin();
    }

    DiskManager* disk_;
    size_t pool_size_;
    vector<Page> frames_;
    unordered_map<PageId, size_t> page_table_;  // pid -> frame idx
    list<size_t> free_list_;
    list<size_t> lru_list_;                     // front = MRU, back = LRU
    unordered_map<size_t, list<size_t>::iterator> lru_pos_;
    mutex latch_;
};

}  // namespace minidb
