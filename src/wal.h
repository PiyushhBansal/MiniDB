// write-ahead log. log the change before it hits the data file. used for crash recovery.
// each record is a before/after image of one tuple op. text format, one record per line.
#pragma once

#include "common.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <unordered_set>
#include <unordered_map>

using namespace std;

namespace minidb {

enum class LogType { BEGIN, INSERT, DELETE, UPDATE, COMMIT, ABORT, CHECKPOINT };

inline string log_type_str(LogType t) {
    switch (t) {
        case LogType::BEGIN: return "BEGIN";
        case LogType::INSERT: return "INSERT";
        case LogType::DELETE: return "DELETE";
        case LogType::UPDATE: return "UPDATE";
        case LogType::COMMIT: return "COMMIT";
        case LogType::ABORT: return "ABORT";
        case LogType::CHECKPOINT: return "CHECKPOINT";
    }
    return "?";
}
inline LogType parse_log_type(const string& s) {
    if (s == "BEGIN") return LogType::BEGIN;
    if (s == "INSERT") return LogType::INSERT;
    if (s == "DELETE") return LogType::DELETE;
    if (s == "UPDATE") return LogType::UPDATE;
    if (s == "COMMIT") return LogType::COMMIT;
    if (s == "ABORT") return LogType::ABORT;
    return LogType::CHECKPOINT;
}

struct LogRecord {
    Lsn lsn = 0;
    TxnId txn = 0;
    LogType type = LogType::BEGIN;
    string table;
    PageId page = INVALID_PAGE_ID;
    int16_t slot = -1;
    string before;  // before-image, for undo
    string after;   // after-image, for redo
};

// base64 so the binary tuple bytes survive in the text log.
namespace b64 {
    inline const char* tbl() { return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; }
    inline string encode(const string& in) {
        string out; int val = 0, bits = -6; const char* T = tbl();
        for (unsigned char c : in) {
            val = (val << 8) + c; bits += 8;
            while (bits >= 0) { out += T[(val >> bits) & 0x3F]; bits -= 6; }
        }
        if (bits > -6) out += T[((val << 8) >> (bits + 8)) & 0x3F];
        while (out.size() % 4) out += '=';
        return out;
    }
    inline string decode(const string& in) {
        vector<int> rev(256, -1); const char* T = tbl();
        for (int k = 0; k < 64; ++k) rev[(unsigned char)T[k]] = k;
        string out; int val = 0, bits = -8;
        for (unsigned char c : in) {
            if (c == '=' || rev[c] == -1) break;
            val = (val << 6) + rev[c]; bits += 6;
            if (bits >= 0) { out += char((val >> bits) & 0xFF); bits -= 8; }
        }
        return out;
    }
}

class WAL {
public:
    explicit WAL(const string& db_file) : path_(db_file + ".wal") {
        // next lsn = count of existing records
        ifstream in(path_);
        string line;
        while (getline(in, line)) if (!line.empty()) next_lsn_++;
        log_.open(path_, ios::app);
    }

    // append a record, return its lsn. caller flushes on commit.
    Lsn append(TxnId txn, LogType type, const string& table = "",
               PageId page = INVALID_PAGE_ID, int16_t slot = -1,
               const string& before = "", const string& after = "") {
        Lsn lsn = ++next_lsn_;
        log_ << lsn << '|' << txn << '|' << log_type_str(type) << '|' << table << '|'
             << page << '|' << slot << '|' << b64::encode(before) << '|'
             << b64::encode(after) << '\n';
        return lsn;
    }

    // push log to disk. called on commit - that's where durability comes from.
    void flush() { log_.flush(); }

    // read the whole log back, recovery uses this.
    vector<LogRecord> read_all() {
        vector<LogRecord> recs;
        ifstream in(path_);
        string line;
        while (getline(in, line)) {
            if (line.empty()) continue;
            vector<string> f = split(line, '|');
            if (f.size() < 8) continue;
            LogRecord r;
            r.lsn = stoull(f[0]);
            r.txn = stoull(f[1]);
            r.type = parse_log_type(f[2]);
            r.table = f[3];
            r.page = stoi(f[4]);
            r.slot = (int16_t)stoi(f[5]);
            r.before = b64::decode(f[6]);
            r.after = b64::decode(f[7]);
            recs.push_back(move(r));
        }
        return recs;
    }

    void checkpoint() { append(INVALID_TXN_ID, LogType::CHECKPOINT); flush(); }

private:
    static vector<string> split(const string& s, char d) {
        vector<string> out; string cur; istringstream ss(s);
        while (getline(ss, cur, d)) out.push_back(cur);
        // getline drops a trailing empty field, pad back to 8
        while (out.size() < 8) out.push_back("");
        return out;
    }

    string path_;
    ofstream log_;
    Lsn next_lsn_ = 0;
};

}  // namespace minidb
