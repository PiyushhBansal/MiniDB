// values + schemas + tuple serialization. only INT and VARCHAR for now.
// on-disk layout: int = 8 bytes, varchar = 4 byte len + bytes. no type tags,
// schema comes from the catalog.
#pragma once

#include "common.h"
#include <string>
#include <vector>
#include <variant>
#include <cstring>

using namespace std;

namespace minidb {

enum class Type { INT, VARCHAR };

inline string type_name(Type t) {
    return t == Type::INT ? "INT" : "VARCHAR";
}

struct Value {
    Type type = Type::INT;
    int64_t i = 0;
    string s;

    Value() = default;
    static Value Int(int64_t v) { Value val; val.type = Type::INT; val.i = v; return val; }
    static Value Str(string v) { Value val; val.type = Type::VARCHAR; val.s = std::move(v); return val; }

    // used by indexes / ORDER BY. different types -> just compare the tag so
    // it's still a total order
    int compare(const Value& o) const {
        if (type != o.type) return type < o.type ? -1 : 1;
        if (type == Type::INT) return i < o.i ? -1 : (i > o.i ? 1 : 0);
        return s.compare(o.s) < 0 ? -1 : (s == o.s ? 0 : 1);
    }
    bool operator==(const Value& o) const { return compare(o) == 0; }
    bool operator<(const Value& o) const { return compare(o) < 0; }

    string to_string() const {
        return type == Type::INT ? std::to_string(i) : s;
    }
};

struct Column {
    string name;
    Type type;
};

struct Schema {
    vector<Column> columns;

    int index_of(const string& col) const {
        for (size_t i = 0; i < columns.size(); ++i)
            if (columns[i].name == col) return static_cast<int>(i);
        return -1;
    }
    size_t size() const { return columns.size(); }
};

using Tuple = vector<Value>;

inline string serialize_tuple(const Tuple& t, const Schema& schema) {
    string out;
    for (size_t c = 0; c < schema.columns.size(); ++c) {
        if (schema.columns[c].type == Type::INT) {
            int64_t v = t[c].i;
            out.append(reinterpret_cast<const char*>(&v), sizeof(v));
        } else {
            int32_t len = static_cast<int32_t>(t[c].s.size());
            out.append(reinterpret_cast<const char*>(&len), sizeof(len));
            out.append(t[c].s);
        }
    }
    return out;
}

inline Tuple deserialize_tuple(const char* data, size_t len, const Schema& schema) {
    Tuple t;
    size_t off = 0;
    for (const auto& col : schema.columns) {
        if (col.type == Type::INT) {
            int64_t v = 0;
            memcpy(&v, data + off, sizeof(v));
            off += sizeof(v);
            t.push_back(Value::Int(v));
        } else {
            int32_t slen = 0;
            memcpy(&slen, data + off, sizeof(slen));
            off += sizeof(slen);
            t.push_back(Value::Str(string(data + off, data + off + slen)));
            off += slen;
        }
    }
    (void)len;
    return t;
}

// ---- MVCC version header ----
// for mvcc tables we put a small header in front of each stored tuple:
//   [ xmin (8 bytes) ][ xmax (8 bytes) ][ normal tuple bytes... ]
// xmin = txn that created this version, xmax = txn that deleted it (0 = still live).
struct VersionHeader {
    TxnId xmin = 0;
    TxnId xmax = 0;
};
constexpr size_t VHDR_SIZE = 16;   // two uint64

inline string serialize_versioned(const Tuple& t, const Schema& schema, TxnId xmin, TxnId xmax) {
    string out;
    out.append(reinterpret_cast<const char*>(&xmin), sizeof(xmin));
    out.append(reinterpret_cast<const char*>(&xmax), sizeof(xmax));
    out += serialize_tuple(t, schema);   // header then the usual column bytes
    return out;
}

inline VersionHeader read_version_header(const char* data) {
    VersionHeader vh;
    memcpy(&vh.xmin, data, sizeof(vh.xmin));
    memcpy(&vh.xmax, data + sizeof(vh.xmin), sizeof(vh.xmax));
    return vh;
}

// overwrite just the xmax field in an already-serialized versioned image
inline void set_xmax(string& image, TxnId xmax) {
    memcpy(&image[sizeof(TxnId)], &xmax, sizeof(xmax));
}

}  // namespace minidb
