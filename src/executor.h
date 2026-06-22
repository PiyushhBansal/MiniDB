// volcano-style operators: each one has next() and they stack into a tree.
// e.g. a select becomes Project <- Sort <- Filter <- (Join | Scan).
#pragma once

#include "catalog.h"
#include "heap_file.h"
#include "bplus_tree.h"
#include "lsm_tree.h"
#include "ast.h"
#include "optimizer.h"
#include <memory>
#include <algorithm>
#include <functional>

using namespace std;

namespace minidb {

struct Row {
    Tuple values;
    RecordId rid;  // only set for base-table rows, DELETE needs it
};

struct Operator {
    Schema out_schema;
    virtual ~Operator() = default;
    virtual void open() = 0;
    virtual bool next(Row& out) = 0;
    virtual void close() {}
};

inline bool eval_pred(const Predicate& p, const Row& row, const Schema& schema) {
    // column might be "table.col" or just "col", try both
    int idx = schema.index_of(p.column);
    if (idx < 0) {
        auto dot = p.column.find('.');
        if (dot != string::npos) idx = schema.index_of(p.column.substr(dot + 1));
    }
    if (idx < 0) return false;
    int c = row.values[idx].compare(p.literal);
    switch (p.op) {
        case CmpOp::EQ: return c == 0;
        case CmpOp::NE: return c != 0;
        case CmpOp::LT: return c < 0;
        case CmpOp::LE: return c <= 0;
        case CmpOp::GT: return c > 0;
        case CmpOp::GE: return c >= 0;
    }
    return false;
}

struct SeqScan : Operator {
    BufferPool* bp;
    TableInfo info;
    vector<Row> buffered;
    size_t cursor = 0;

    SeqScan(BufferPool* bp_, const TableInfo& ti) : bp(bp_), info(ti) {
        out_schema = ti.schema;
    }
    void open() override {
        HeapFile hf(bp, info.heap_root);
        hf.scan([&](RecordId rid, const string& rec) {
            Row r;
            r.values = deserialize_tuple(rec.data(), rec.size(), info.schema);
            r.rid = rid;
            buffered.push_back(move(r));
            return true;
        });
        cursor = 0;
    }
    bool next(Row& out) override {
        if (cursor >= buffered.size()) return false;
        out = buffered[cursor++];
        return true;
    }
};

// merged ordered scan when the table is backed by the LSM engine
struct LsmScan : Operator {
    LSMTree* lsm;
    Schema schema;
    vector<Row> buffered;
    size_t cursor = 0;
    LsmScan(LSMTree* l, const Schema& s) : lsm(l), schema(s) { out_schema = s; }
    void open() override {
        lsm->scan([&](const string&, const string& rec) {
            Row r;
            r.values = deserialize_tuple(rec.data(), rec.size(), schema);
            buffered.push_back(move(r));
            return true;
        });
        cursor = 0;
    }
    bool next(Row& out) override {
        if (cursor >= buffered.size()) return false;
        out = buffered[cursor++];
        return true;
    }
};

// walk the PK b+tree for the range, then grab the matching heap tuples
struct IndexScan : Operator {
    BufferPool* bp;
    TableInfo info;
    int64_t lo, hi;  // inclusive
    vector<Row> buffered;
    size_t cursor = 0;

    IndexScan(BufferPool* bp_, const TableInfo& ti, int64_t lo_, int64_t hi_)
        : bp(bp_), info(ti), lo(lo_), hi(hi_) {
        out_schema = ti.schema;
    }
    void open() override {
        BPlusTree tree(bp, info.index_root);
        HeapFile hf(bp, info.heap_root);
        tree.range_scan(lo, hi, [&](int64_t, RecordId rid) {
            auto rec = hf.get(rid);
            if (rec) {
                Row r;
                r.values = deserialize_tuple(rec->data(), rec->size(), info.schema);
                r.rid = rid;
                buffered.push_back(move(r));
            }
            return true;
        });
        cursor = 0;
    }
    bool next(Row& out) override {
        if (cursor >= buffered.size()) return false;
        out = buffered[cursor++];
        return true;
    }
};

struct Filter : Operator {
    unique_ptr<Operator> child;
    vector<Predicate> preds;
    Filter(unique_ptr<Operator> c, vector<Predicate> p)
        : child(move(c)), preds(move(p)) { out_schema = child->out_schema; }
    void open() override { child->open(); }
    bool next(Row& out) override {
        Row r;
        while (child->next(r)) {
            bool ok = true;
            for (auto& p : preds) if (!eval_pred(p, r, out_schema)) { ok = false; break; }
            if (ok) { out = r; return true; }
        }
        return false;
    }
    void close() override { child->close(); }
};

// inner equi-join. build the hash table on the right (smaller) side and probe
// with the left so the table we keep in memory stays small.
struct HashJoin : Operator {
    unique_ptr<Operator> left, right;
    string left_key, right_key;
    unordered_multimap<string, Row> build;
    vector<Row> output;
    size_t cursor = 0;

    HashJoin(unique_ptr<Operator> l, unique_ptr<Operator> r,
             string lk, string rk)
        : left(move(l)), right(move(r)), left_key(move(lk)), right_key(move(rk)) {
        for (auto& c : left->out_schema.columns) out_schema.columns.push_back(c);
        for (auto& c : right->out_schema.columns) out_schema.columns.push_back(c);
    }
    void open() override {
        // build phase: hash the right side
        right->open();
        int rk = right->out_schema.index_of(right_key);
        Row r;
        while (right->next(r)) build.emplace(r.values[rk].to_string(), r);
        right->close();

        // probe phase
        left->open();
        int lk = left->out_schema.index_of(left_key);
        Row l;
        while (left->next(l)) {
            auto range = build.equal_range(l.values[lk].to_string());
            for (auto it = range.first; it != range.second; ++it) {
                Row joined;
                joined.values = l.values;
                joined.values.insert(joined.values.end(), it->second.values.begin(), it->second.values.end());
                output.push_back(move(joined));
            }
        }
        left->close();
        cursor = 0;
    }
    bool next(Row& out) override {
        if (cursor >= output.size()) return false;
        out = output[cursor++];
        return true;
    }
};

// ORDER BY. just buffers everything and stable_sorts it
struct Sort : Operator {
    unique_ptr<Operator> child;
    int sort_col;
    bool desc;
    vector<Row> rows;
    size_t cursor = 0;
    Sort(unique_ptr<Operator> c, int col, bool d)
        : child(move(c)), sort_col(col), desc(d) { out_schema = child->out_schema; }
    void open() override {
        child->open();
        Row r;
        while (child->next(r)) rows.push_back(r);
        child->close();
        stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
            int c = a.values[sort_col].compare(b.values[sort_col]);
            return desc ? c > 0 : c < 0;
        });
        cursor = 0;
    }
    bool next(Row& out) override {
        if (cursor >= rows.size()) return false;
        out = rows[cursor++];
        return true;
    }
};

}  // namespace minidb
