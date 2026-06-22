// cost-based optimizer: pick index vs seq scan, and which side to build the hash join on.
// uses the textbook System-R style estimates (equality = 1/rows, range = 1/3).
#pragma once

#include "catalog.h"
#include "ast.h"
#include <cmath>

using namespace std;

namespace minidb {

constexpr double TUPLES_PER_PAGE = 64.0;

struct CostModel {
    static double selectivity(const Predicate& p, const TableInfo& t) {
        double rows = max<int64_t>(t.row_count, 1);
        switch (p.op) {
            case CmpOp::EQ:
                return 1.0 / rows;  // EQ on PK -> assume ~1 row
            case CmpOp::NE:
                return 1.0 - 1.0 / rows;
            default:
                return 1.0 / 3.0;  // 1/3 for ranges, textbook guess
        }
    }

    static double estimate_rows(const TableInfo& t, const vector<Predicate>& preds) {
        double rows = max<int64_t>(t.row_count, 1);
        for (auto& p : preds) {
            // only count preds on this table's columns
            string col = p.column;
            auto dot = col.find('.');
            if (dot != string::npos) col = col.substr(dot + 1);
            if (t.schema.index_of(col) >= 0) rows *= selectivity(p, t);
        }
        return max(rows, 1.0);
    }

    static double seq_scan_cost(const TableInfo& t) {
        return ceil(max<int64_t>(t.row_count, 1) / TUPLES_PER_PAGE);
    }

    // log descent down the b+tree + one fetch per matching row
    static double index_scan_cost(const TableInfo& t, double matching) {
        double n = max<int64_t>(t.row_count, 1);
        return log2(n + 1) + matching;
    }
};

struct AccessPath {
    bool use_index = false;
    int64_t key_lo = 0, key_hi = 0;  // PK range if use_index
    double est_rows = 0;
    double est_cost = 0;
    string reason;  // shown by EXPLAIN
};

inline AccessPath choose_access_path(const TableInfo& t, const vector<Predicate>& preds) {
    AccessPath path;
    double seq_cost = CostModel::seq_scan_cost(t);
    path.est_rows = CostModel::estimate_rows(t, preds);

    // try to find a PK predicate we can turn into a range scan
    if (t.index_root != INVALID_PAGE_ID && t.pk_col >= 0) {
        const string& pk = t.schema.columns[t.pk_col].name;
        for (auto& p : preds) {
            string col = p.column;
            auto dot = col.find('.');
            if (dot != string::npos) col = col.substr(dot + 1);
            if (col != pk || p.literal.type != Type::INT) continue;
            int64_t v = p.literal.i;
            int64_t lo = numeric_limits<int64_t>::min();
            int64_t hi = numeric_limits<int64_t>::max();
            double matching = 1;
            switch (p.op) {
                case CmpOp::EQ: lo = hi = v; matching = 1; break;
                case CmpOp::GE: lo = v; matching = path.est_rows; break;
                case CmpOp::GT: lo = v + 1; matching = path.est_rows; break;
                case CmpOp::LE: hi = v; matching = path.est_rows; break;
                case CmpOp::LT: hi = v - 1; matching = path.est_rows; break;
                default: continue;  // NE isn't worth an index scan
            }
            double idx_cost = CostModel::index_scan_cost(t, matching);
            if (idx_cost < seq_cost) {
                path.use_index = true;
                path.key_lo = lo; path.key_hi = hi;
                path.est_cost = idx_cost;
                path.reason = "index scan on PK '" + pk + "' (cost " +
                              to_string(idx_cost) + " < seq " + to_string(seq_cost) + ")";
                return path;
            }
        }
    }
    path.use_index = false;
    path.est_cost = seq_cost;
    path.reason = "sequential scan (cost " + to_string(seq_cost) + ")";
    return path;
}

}  // namespace minidb
