// table/index metadata. just remembers where each table lives + its columns.
// kept as a tiny text sidecar (<db>.catalog), not an on-page format - it's
// small and rarely changes so a readable file is easier to debug. not WAL'd.
#pragma once

#include "common.h"
#include "tuple.h"
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <memory>

using namespace std;

namespace minidb {

// HEAP = heap file + B+ tree, locked reads (default).
// MVCC = same heap storage but versioned rows + snapshot reads (extension track B).
enum class StorageKind { HEAP, MVCC };

struct TableInfo {
    string name;
    Schema schema;
    StorageKind storage = StorageKind::HEAP;
    PageId heap_root = INVALID_PAGE_ID;  // first page of the heap file
    int pk_col = -1;                     // pk column idx, -1 = none
    PageId index_root = INVALID_PAGE_ID;  // B+ tree root over the pk
    int64_t row_count = 0;               // for the optimizer's cost model
};

class Catalog {
public:
    explicit Catalog(const string& db_file) : path_(db_file + ".catalog") {
        load();
    }

    bool has_table(const string& name) const {
        return tables_.count(name) > 0;
    }

    TableInfo* get(const string& name) {
        auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : &it->second;
    }

    void put(const TableInfo& t) {
        tables_[t.name] = t;
        save();
    }

    void update(const TableInfo& t) { put(t); }

    vector<string> table_names() const {
        vector<string> out;
        for (auto& [k, _] : tables_) out.push_back(k);
        return out;
    }

private:
    // one line per table, '|' separated:
    //   T|name|pk_col|heap_root|index_root|row_count|col:type,col:type,...|storage
    void save() {
        ofstream f(path_, ios::trunc);
        for (auto& [name, t] : tables_) {
            f << "T|" << t.name << '|' << t.pk_col << '|' << t.heap_root << '|'
              << t.index_root << '|' << t.row_count << '|';
            for (size_t i = 0; i < t.schema.columns.size(); ++i) {
                if (i) f << ',';
                f << t.schema.columns[i].name << ':'
                  << (t.schema.columns[i].type == Type::INT ? "INT" : "VARCHAR");
            }
            // storage goes last so older catalogs without it still load
            f << '|' << (t.storage == StorageKind::MVCC ? "MVCC" : "HEAP");
            f << '\n';
        }
    }

    void load() {
        ifstream f(path_);
        if (!f.is_open()) return;
        string line;
        while (getline(f, line)) {
            if (line.empty() || line[0] != 'T') continue;
            vector<string> parts = split(line, '|');
            if (parts.size() < 7) continue;
            TableInfo t;
            t.name = parts[1];
            t.pk_col = stoi(parts[2]);
            t.heap_root = stoi(parts[3]);
            t.index_root = stoi(parts[4]);
            t.row_count = stoll(parts[5]);
            for (auto& colspec : split(parts[6], ',')) {
                auto kv = split(colspec, ':');
                if (kv.size() != 2) continue;
                Column c;
                c.name = kv[0];
                c.type = (kv[1] == "INT") ? Type::INT : Type::VARCHAR;
                t.schema.columns.push_back(c);
            }
            if (parts.size() >= 8 && parts[7] == "MVCC") t.storage = StorageKind::MVCC;
            tables_[t.name] = t;
        }
    }

    static vector<string> split(const string& s, char d) {
        vector<string> out;
        string cur;
        istringstream ss(s);
        while (getline(ss, cur, d)) out.push_back(cur);
        return out;
    }

    string path_;
    unordered_map<string, TableInfo> tables_;
};

}  // namespace minidb
