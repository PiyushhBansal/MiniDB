// lsm storage engine (alternative to the b+tree/heap path).
// write-optimized: buffer writes in memory, flush to sorted sstables. the cost
// is reads have to check the memtable + every sstable. keys/vals are strings.
#pragma once

#include "common.h"
#include <map>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <optional>
#include <algorithm>
#include <memory>

using namespace std;

namespace minidb {

// a value plus a tombstone flag. tombstone = key was deleted, hides older values.
struct LsmValue {
    string data;
    bool tombstone = false;
};

// immutable sorted run on disk. line format: <keylen> <key> <flag> <vallen> <val>.
// we keep a full key->offset index in memory (fine for our small data sets).
class SSTable {
public:
    explicit SSTable(string path) : path_(move(path)) {}

    static shared_ptr<SSTable> create(const string& path,
                                           const map<string, LsmValue>& data) {
        ofstream out(path, ios::binary | ios::trunc);
        auto sst = make_shared<SSTable>(path);
        for (auto& [k, v] : data) {
            int64_t off = out.tellp();
            out << k.size() << ' ' << k << ' ' << (v.tombstone ? 1 : 0) << ' '
                << v.data.size() << ' ' << v.data << '\n';
            sst->index_[k] = off;
            sst->min_key_ = sst->min_key_.empty() ? k : sst->min_key_;
            sst->max_key_ = k;
        }
        out.flush();
        return sst;
    }

    // rebuild the in-memory index from the file (after restart)
    void load_index() {
        ifstream in(path_, ios::binary);
        string line;
        while (true) {
            int64_t off = in.tellg();
            if (!getline(in, line)) break;
            istringstream ss(line);
            size_t klen; ss >> klen; ss.get();
            string key(klen, '\0'); ss.read(&key[0], klen);
            index_[key] = off;
            if (min_key_.empty()) min_key_ = key;
            max_key_ = key;
        }
    }

    optional<LsmValue> get(const string& key) {
        auto it = index_.find(key);
        if (it == index_.end()) return nullopt;
        ifstream in(path_, ios::binary);
        in.seekg(it->second);
        return read_entry(in);
    }

    // read everything in key order, used by compaction
    vector<pair<string, LsmValue>> scan() {
        vector<pair<string, LsmValue>> out;
        ifstream in(path_, ios::binary);
        string line;
        while (in.peek() != EOF) {
            auto e = read_entry(in);
            if (e) out.push_back({last_key_, *e});
        }
        return out;
    }

    const string& path() const { return path_; }
    size_t size() const { return index_.size(); }

private:
    optional<LsmValue> read_entry(ifstream& in) {
        size_t klen;
        if (!(in >> klen)) return nullopt;
        in.get();  // space
        string key(klen, '\0'); in.read(&key[0], klen);
        in.get();  // space
        int flag; in >> flag; in.get();
        size_t vlen; in >> vlen; in.get();
        string val(vlen, '\0'); in.read(&val[0], vlen);
        in.get();  // newline
        last_key_ = key;
        return LsmValue{val, flag != 0};
    }

    string path_;
    map<string, int64_t> index_;  // key -> file offset
    string min_key_, max_key_;
    string last_key_;
};

class LSMTree {
public:
    // flush the memtable at memtable_limit keys, compact at compaction_threshold sstables
    explicit LSMTree(string dir, size_t memtable_limit = 1000,
                     size_t compaction_threshold = 4)
        : dir_(move(dir)), memtable_limit_(memtable_limit),
          compaction_threshold_(compaction_threshold) {}

    void put(const string& key, const string& value) {
        memtable_[key] = LsmValue{value, false};
        maybe_flush();
    }

    void remove(const string& key) {
        memtable_[key] = LsmValue{"", true};  // tombstone
        maybe_flush();
    }

    // memtable first, then sstables newest->oldest. first hit wins, tombstone means deleted
    optional<string> get(const string& key) {
        auto it = memtable_.find(key);
        if (it != memtable_.end())
            return it->second.tombstone ? nullopt : optional<string>(it->second.data);
        for (auto rit = sstables_.rbegin(); rit != sstables_.rend(); ++rit) {
            auto v = (*rit)->get(key);
            if (v) return v->tombstone ? nullopt : optional<string>(v->data);
        }
        return nullopt;
    }

    // full ordered scan: merge memtable + all sstables, newest wins, skip tombstones
    template <typename Fn>
    void scan(Fn&& fn) {
        map<string, LsmValue> merged;
        // oldest first so newer writes overwrite in the map
        for (auto& sst : sstables_)
            for (auto& [k, v] : sst->scan()) merged[k] = v;
        for (auto& [k, v] : memtable_) merged[k] = v;
        for (auto& [k, v] : merged)
            if (!v.tombstone)
                if (!fn(k, v.data)) return;
    }

    size_t num_sstables() const { return sstables_.size(); }
    size_t memtable_size() const { return memtable_.size(); }
    int compactions_run() const { return compactions_; }

    // dump the current memtable to a new sstable (also called on shutdown)
    void flush_memtable() {
        if (memtable_.empty()) return;
        string path = dir_ + "/sst_" + to_string(next_sst_id_++) + ".sst";
        sstables_.push_back(SSTable::create(path, memtable_));
        memtable_.clear();
    }

private:
    void maybe_flush() {
        if (memtable_.size() >= memtable_limit_) {
            flush_memtable();
            if (sstables_.size() >= compaction_threshold_) compact();
        }
    }

    // merge all sstables into one, newest wins. drops tombstones since nothing
    // older survives this merge anyway.
    void compact() {
        map<string, LsmValue> merged;
        for (auto& sst : sstables_)              // oldest -> newest
            for (auto& [k, v] : sst->scan()) merged[k] = v;
        for (auto it = merged.begin(); it != merged.end();) {
            if (it->second.tombstone) it = merged.erase(it);
            else ++it;
        }
        for (auto& sst : sstables_) remove(sst->path().c_str());
        sstables_.clear();
        string path = dir_ + "/sst_" + to_string(next_sst_id_++) + ".sst";
        sstables_.push_back(SSTable::create(path, merged));
        compactions_++;
    }

    string dir_;
    size_t memtable_limit_, compaction_threshold_;
    map<string, LsmValue> memtable_;
    vector<shared_ptr<SSTable>> sstables_;  // oldest .. newest
    int next_sst_id_ = 0;
    int compactions_ = 0;
};

}  // namespace minidb
