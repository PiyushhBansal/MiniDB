// AST the parser spits out and the planner reads. one struct per stmt kind.
#pragma once

#include "tuple.h"
#include <string>
#include <vector>
#include <memory>

using namespace std;

namespace minidb {

enum class CmpOp { EQ, NE, LT, LE, GT, GE };

// one <col> <op> <literal>. only AND'd together, no OR.
struct Predicate {
    string column;  // "table.col" in a join
    CmpOp op;
    Value literal;
};

struct CreateStmt {
    string table;
    Schema schema;
    string pk_column;  // empty if none
    bool use_mvcc = false;
};

struct InsertStmt {
    string table;
    vector<Tuple> rows;
};

// SELECT cols FROM t [JOIN t2 ON a=b] [WHERE ...]
struct SelectStmt {
    vector<string> columns;   // empty or {"*"} = all

    string table;                  // left table
    bool has_join = false;              // one inner join max
    string join_table;
    string join_left_col;
    string join_right_col;

    vector<Predicate> where;
};

struct DeleteStmt {
    string table;
    vector<Predicate> where;
};

// a parsed stmt is exactly one of these
struct Statement {
    enum Kind { CREATE, INSERT, SELECT, DELETE_, BEGIN_TXN, COMMIT_TXN, ROLLBACK_TXN, NONE } kind = NONE;
    CreateStmt create;
    InsertStmt insert;
    SelectStmt select;
    DeleteStmt del;
};

}  // namespace minidb
