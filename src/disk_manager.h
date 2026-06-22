// DiskManager: read/write fixed 4KB pages to one file.
// page N is at byte offset N*PAGE_SIZE. allocate = append.
// lowest layer - knows nothing about tuples/indexes/txns.
#pragma once

#include "common.h"
#include <fstream>
#include <mutex>

using namespace std;

namespace minidb {

class DiskManager {
public:
    explicit DiskManager(const string& db_file) : file_name_(db_file) {
        // open rw, create the file if it isn't there yet
        file_.open(db_file, ios::binary | ios::in | ios::out);
        if (!file_.is_open()) {
            file_.clear();
            file_.open(db_file, ios::binary | ios::out);  // create
            file_.close();
            file_.open(db_file, ios::binary | ios::in | ios::out);
        }
        if (!file_.is_open()) throw DBError("DiskManager: cannot open " + db_file);
        file_.seekg(0, ios::end);
        auto bytes = static_cast<int64_t>(file_.tellg());
        num_pages_ = bytes <= 0 ? 0 : static_cast<PageId>(bytes / PAGE_SIZE);
    }

    ~DiskManager() { if (file_.is_open()) file_.close(); }

    // out must be >= PAGE_SIZE. reading an unwritten page gives zeros.
    void read_page(PageId pid, char* out) {
        lock_guard<mutex> lock(latch_);
        int64_t offset = static_cast<int64_t>(pid) * PAGE_SIZE;
        file_.seekg(offset);
        file_.read(out, PAGE_SIZE);
        // short read past EOF -> zero the rest so the page looks clean
        auto got = file_.gcount();
        if (got < PAGE_SIZE) fill(out + got, out + PAGE_SIZE, 0);
        file_.clear();
    }

    // no fsync here on purpose - durability comes from the WAL (flushed at
    // commit). fsyncing every page write would be super slow under eviction.
    // call sync() when we actually need the data file on disk (checkpoint).
    void write_page(PageId pid, const char* data) {
        lock_guard<mutex> lock(latch_);
        int64_t offset = static_cast<int64_t>(pid) * PAGE_SIZE;
        file_.seekp(offset);
        file_.write(data, PAGE_SIZE);
        if (pid >= num_pages_) num_pages_ = pid + 1;
    }

    void sync() {
        lock_guard<mutex> lock(latch_);
        file_.flush();
    }

    // new page by extending the file
    PageId allocate_page() {
        lock_guard<mutex> lock(latch_);
        PageId pid = num_pages_++;
        char zero[PAGE_SIZE] = {0};   // write zeros so file length grows
        int64_t offset = static_cast<int64_t>(pid) * PAGE_SIZE;
        file_.seekp(offset);
        file_.write(zero, PAGE_SIZE);
        file_.flush();
        return pid;
    }

    PageId num_pages() const { return num_pages_; }

private:
    string file_name_;
    fstream file_;
    PageId num_pages_ = 0;
    mutex latch_;
};

}  // namespace minidb
