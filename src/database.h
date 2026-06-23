// top-level engine. takes a parsed statement and runs it (create/insert/select/delete).
// owns the disk manager, buffer pool, catalog, wal and lock manager.
#pragma once

#include "disk_manager.h"
#include "buffer_pool.h"
#include "catalog.h"
#include "heap_file.h"
#include "bplus_tree.h"
#include "parser.h"
#include "executor.h"
#include "optimizer.h"
#include "wal.h"
#include "recovery.h"
#include "lock_manager.h"
#include "lsm_tree.h"
#include <iostream>
#include <sstream>
#include <map>

using namespace std;

namespace minidb {

// turn an int64 PK into a sortable 8-byte key for the LSM. flip sign bit so
// negatives sort first, then big-endian so byte order matches numeric order.
inline string lsm_key(int64_t v) {
    uint64_t u = (uint64_t)v ^ 0x8000000000000000ULL;
    string k(8, '\0');
    for (int b = 0; b < 8; ++b) k[b] = char((u >> (8 * (7 - b))) & 0xFF);
    return k;
}

struct ResultSet {
    vector<string> columns;
    vector<Tuple> rows;
    string message;        // for non-select / explain
    int64_t affected = 0;
};

class Database {
public:
    explicit Database(const string& db_file)
        : disk_(db_file), pool_(&disk_, /*frames=*/256), catalog_(db_file),
          wal_(db_file) {
        // recover before serving any query. redo committed + uncommitted, then undo the losers.
        Recovery rec(&wal_, &pool_);
        last_recovery_ = rec.recover();
    }

    BufferPool* pool() { return &pool_; }
    Catalog* catalog() { return &catalog_; }
    WAL* wal() { return &wal_; }
    LockManager* locks() { return &lock_mgr_; }
    void flush() { pool_.flush_all(); }

    // shell prints this after startup ("recovered N, rolled back M")
    Recovery::Report recovery_report() const { return last_recovery_; }

    // autocommit by default - each stmt is its own txn. BEGIN opens an explicit one.
    TxnId begin_txn() {
        TxnId t = ++txn_counter_;
        wal_.append(t, LogType::BEGIN);
        return t;
    }
    void commit_txn(TxnId t) {
        wal_.append(t, LogType::COMMIT);
        wal_.flush();              // only flush the log here, not the data pages.
        // durability comes from the wal: even if dirty pages are still in the buffer
        // pool at crash, redo replays them from the log. one sequential log flush
        // instead of random page writes. pages get written lazily later.
        lock_mgr_.release_all(t);  // strict 2PL: release only now, at commit
    }

    // clean shutdown calls this, crash skips it so recovery rebuilds from the wal.
    void checkpoint() {
        pool_.flush_all();
        for (auto& [name, store] : lsm_) store->flush_memtable();
        wal_.checkpoint();
    }
    void abort_txn(TxnId t) {
        // undo this txn's changes from its before-images, reverse order.
        // flush first or read_all won't see records still sitting in the ofstream buffer.
        wal_.flush();
        vector<LogRecord> log = wal_.read_all();
        Recovery rec(&wal_, &pool_);
        for (auto it = log.rbegin(); it != log.rend(); ++it) {
            if (it->txn != t) continue;
            if (it->type == LogType::INSERT) undo_insert(*it);
            else if (it->type == LogType::DELETE) undo_delete(*it);
        }
        wal_.append(t, LogType::ABORT);
        wal_.flush();
        pool_.flush_all();
        lock_mgr_.release_all(t);
    }

    ResultSet execute(const string& sql) {
        // peel off a leading EXPLAIN if present, run the rest
        string trimmed = sql;
        bool explain = false;
        {
            istringstream ss(sql);
            string first; ss >> first;
            string up = first;
            for (auto& c : up) c = toupper((unsigned char)c);
            if (up == "EXPLAIN") {
                explain = true;
                size_t kw = sql.find(first);
                size_t after = kw + first.size();
                trimmed = sql.substr(after);
            }
        }
        Statement st = Parser(trimmed).parse();
        switch (st.kind) {
            case Statement::BEGIN_TXN: {
                if (explicit_txn_) throw DBError("transaction already in progress");
                explicit_txn_ = begin_txn();
                ResultSet rs; rs.message = "BEGIN (txn " + to_string(explicit_txn_) + ")";
                return rs;
            }
            case Statement::COMMIT_TXN: {
                if (!explicit_txn_) throw DBError("no transaction in progress");
                TxnId t = explicit_txn_; explicit_txn_ = 0;
                commit_txn(t);
                ResultSet rs; rs.message = "COMMIT (txn " + to_string(t) + ")";
                return rs;
            }
            case Statement::ROLLBACK_TXN: {
                if (!explicit_txn_) throw DBError("no transaction in progress");
                TxnId t = explicit_txn_; explicit_txn_ = 0;
                abort_txn(t);
                ResultSet rs; rs.message = "ROLLBACK (txn " + to_string(t) + ")";
                return rs;
            }
            case Statement::CREATE: return exec_create(st.create);
            case Statement::INSERT: return exec_insert(st.insert);
            case Statement::SELECT: return explain ? exec_explain(st.select) : exec_select(st.select);
            case Statement::DELETE_: return exec_delete(st.del);
            default: throw DBError("unsupported statement");
        }
    }

    // gives the txn a stmt runs under: the open explicit one, or a throwaway
    // autocommit one we commit when the scope dies.
    struct TxnScope {
        Database* db; TxnId txn; bool autocommit;
        ~TxnScope() { if (autocommit && txn) db->commit_txn(txn); }
    };
    TxnScope txn_scope() {
        if (explicit_txn_) return {this, explicit_txn_, false};
        return {this, begin_txn(), true};
    }

private:
    ResultSet exec_create(const CreateStmt& c) {
        if (catalog_.has_table(c.table)) throw DBError("table already exists: " + c.table);
        TableInfo t;
        t.name = c.table;
        t.schema = c.schema;
        t.pk_col = c.pk_column.empty() ? -1 : c.schema.index_of(c.pk_column);
        t.row_count = 0;
        if (c.use_lsm) {
            t.storage = StorageKind::LSM;
            lsm_store(t);  // makes the on-disk lsm dir + handle
        } else {
            t.storage = StorageKind::HEAP;
            t.heap_root = HeapFile::create(&pool_);
            if (t.pk_col >= 0) t.index_root = BPlusTree::create(&pool_);
        }
        catalog_.put(t);
        pool_.flush_all();
        ResultSet rs; rs.message = "table '" + c.table + "' created" +
                                   (c.use_lsm ? " (LSM storage)" : "");
        return rs;
    }

    // get the lsm handle, create it on first use
    LSMTree* lsm_store(const TableInfo& t) {
        auto it = lsm_.find(t.name);
        if (it != lsm_.end()) return it->second.get();
        string dir = "minidb_lsm_" + t.name;
        string mk = "mkdir -p '" + dir + "'";
        system(mk.c_str());
        auto store = make_unique<LSMTree>(dir);
        LSMTree* ptr = store.get();
        lsm_[t.name] = move(store);
        return ptr;
    }

    // write row to heap, add to pk index, log it. note the ordering: wal record
    // is appended before the page ever reaches disk - that's the write-ahead rule.
    // X-lock the row and hold it to commit (strict 2PL).
    ResultSet exec_insert(const InsertStmt& ins) {
        TableInfo* t = catalog_.get(ins.table);
        if (!t) throw DBError("no such table: " + ins.table);

        // lsm tables go their own way: just put the tuple keyed by pk. lsm has
        // its own wal so we don't double-log here.
        if (t->storage == StorageKind::LSM) {
            if (t->pk_col < 0) throw DBError("LSM tables require a PRIMARY KEY");
            LSMTree* lsm = lsm_store(*t);
            int64_t n = 0;
            for (const auto& row : ins.rows) {
                if (row.size() != t->schema.size())
                    throw DBError("column count mismatch on INSERT");
                lsm->put(lsm_key(row[t->pk_col].i), serialize_tuple(row, t->schema));
                t->row_count++; n++;
            }
            catalog_.update(*t);
            ResultSet rs; rs.affected = n; rs.message = to_string(n) + " row(s) inserted";
            return rs;
        }

        auto scope = txn_scope();
        HeapFile hf(&pool_, t->heap_root);
        int64_t n = 0;
        for (const auto& row : ins.rows) {
            if (row.size() != t->schema.size())
                throw DBError("column count mismatch on INSERT");
            string image = serialize_tuple(row, t->schema);
            RecordId rid = hf.insert(image);
            lock_mgr_.acquire(scope.txn, ResourceId{rid.page_id, rid.slot}, LockMode::EXCLUSIVE);
            wal_.append(scope.txn, LogType::INSERT, t->name, rid.page_id, rid.slot, "", image);
            if (t->pk_col >= 0) {
                BPlusTree tree(&pool_, t->index_root);
                tree.insert(row[t->pk_col].i, rid);
            }
            t->row_count++;
            n++;
        }
        catalog_.update(*t);
        ResultSet rs; rs.affected = n; rs.message = to_string(n) + " row(s) inserted";
        return rs;
    }

    // builds the operator tree. optimizer picks access path + join order here.
    unique_ptr<Operator> plan_select(const SelectStmt& s, string* explain_out = nullptr) {
        TableInfo* t = catalog_.get(s.table);
        if (!t) throw DBError("no such table: " + s.table);

        // split where preds: ones on the driving table vs the rest
        vector<Predicate> left_preds, other_preds;
        for (auto& p : s.where) {
            string col = p.column;
            auto dot = col.find('.');
            string tbl = dot == string::npos ? "" : col.substr(0, dot);
            string bare = dot == string::npos ? col : col.substr(dot + 1);
            if (!s.has_join || tbl.empty() || tbl == s.table) {
                if (t->schema.index_of(bare) >= 0) left_preds.push_back(p);
                else other_preds.push_back(p);
            } else {
                other_preds.push_back(p);
            }
        }

        // optimizer picks index vs seq scan for the driving table
        AccessPath ap = choose_access_path(*t, left_preds);
        unique_ptr<Operator> driver;
        if (t->storage == StorageKind::LSM) {
            // lsm just has one merged scan, no secondary index to choose
            driver = make_unique<LsmScan>(lsm_store(*t), t->schema);
            if (explain_out) *explain_out += "  " + s.table + ": LSM merged scan\n";
        } else if (ap.use_index) {
            driver = make_unique<IndexScan>(&pool_, *t, ap.key_lo, ap.key_hi);
            if (explain_out) *explain_out += "  " + s.table + ": " + ap.reason + "\n";
        } else {
            driver = make_unique<SeqScan>(&pool_, *t);
            if (explain_out) *explain_out += "  " + s.table + ": " + ap.reason + "\n";
        }

        unique_ptr<Operator> root = move(driver);
        // leftover preds on the driving table become a filter on top
        if (!left_preds.empty())
            root = make_unique<Filter>(move(root), left_preds);

        if (s.has_join) {
            TableInfo* u = catalog_.get(s.join_table);
            if (!u) throw DBError("no such table: " + s.join_table);
            auto right = make_unique<SeqScan>(&pool_, *u);

            // hash join builds on the smaller side, swap if left is smaller
            double left_rows = ap.est_rows;
            double right_rows = (double)max<int64_t>(u->row_count, 1);

            auto bare = [](const string& q) {
                auto d = q.find('.'); return d == string::npos ? q : q.substr(d + 1);
            };
            string lk = bare(s.join_left_col), rk = bare(s.join_right_col);
            // make sure lk is the left table's col, rk the right's
            if (t->schema.index_of(lk) < 0) swap(lk, rk);

            unique_ptr<Operator> join;
            if (explain_out)
                *explain_out += "  join: build on " +
                    (left_rows <= right_rows ? s.table : s.join_table) +
                    " (smaller side, ~" + to_string((int64_t)min(left_rows, right_rows)) + " rows)\n";

            if (left_rows <= right_rows)
                join = make_unique<HashJoin>(move(right), move(root), rk, lk);
            else
                join = make_unique<HashJoin>(move(root), move(right), lk, rk);
            root = move(join);

            if (!other_preds.empty())
                root = make_unique<Filter>(move(root), other_preds);
        } else if (!other_preds.empty()) {
            root = make_unique<Filter>(move(root), other_preds);
        }
        return root;
    }

    ResultSet exec_select(const SelectStmt& s) {
        auto root = plan_select(s);
        root->open();

        ResultSet rs;
        const Schema& osch = root->out_schema;

        // projection: which columns to emit
        vector<int> proj;
        bool star = s.columns.empty() || (s.columns.size() == 1 && s.columns[0] == "*");
        if (star) {
            for (size_t i = 0; i < osch.size(); ++i) proj.push_back((int)i);
        } else {
            for (auto& c : s.columns) {
                int idx = osch.index_of(c);
                if (idx < 0) {
                    auto d = c.find('.');
                    if (d != string::npos) idx = osch.index_of(c.substr(d + 1));
                }
                if (idx < 0) throw DBError("unknown column: " + c);
                proj.push_back(idx);
            }
        }
        for (int idx : proj) rs.columns.push_back(osch.columns[idx].name);

        Row r;
        while (root->next(r)) {
            Tuple out;
            for (int idx : proj) out.push_back(r.values[idx]);
            rs.rows.push_back(move(out));
        }
        root->close();
        return rs;
    }

    ResultSet exec_explain(const SelectStmt& s) {
        string plan = "QUERY PLAN:\n";
        auto root = plan_select(s, &plan);
        (void)root;
        ResultSet rs; rs.message = plan;
        return rs;
    }

    // X-lock matches, log the before-image so undo can bring them back, tombstone, drop pk key.
    ResultSet exec_delete(const DeleteStmt& d) {
        TableInfo* t = catalog_.get(d.table);
        if (!t) throw DBError("no such table: " + d.table);

        // lsm: scan for matches, tombstone each matched pk
        if (t->storage == StorageKind::LSM) {
            LSMTree* lsm = lsm_store(*t);
            vector<int64_t> kill;
            lsm->scan([&](const string&, const string& rec) {
                Tuple tup = deserialize_tuple(rec.data(), rec.size(), t->schema);
                Row row{tup, {}};
                bool match = true;
                for (auto& p : d.where) if (!eval_pred(p, row, t->schema)) { match = false; break; }
                if (match) kill.push_back(tup[t->pk_col].i);
                return true;
            });
            for (int64_t k : kill) { lsm->remove(lsm_key(k)); t->row_count--; }
            catalog_.update(*t);
            ResultSet rs; rs.affected = (int64_t)kill.size();
            rs.message = to_string(kill.size()) + " row(s) deleted";
            return rs;
        }

        auto scope = txn_scope();
        HeapFile hf(&pool_, t->heap_root);
        unique_ptr<BPlusTree> tree;
        if (t->pk_col >= 0) tree = make_unique<BPlusTree>(&pool_, t->index_root);

        vector<RecordId> to_delete;
        vector<int64_t> keys;
        vector<string> images;
        hf.scan([&](RecordId rid, const string& rec) {
            Tuple tup = deserialize_tuple(rec.data(), rec.size(), t->schema);
            Row row{tup, rid};
            bool match = true;
            for (auto& p : d.where) if (!eval_pred(p, row, t->schema)) { match = false; break; }
            if (match) {
                to_delete.push_back(rid);
                images.push_back(rec);
                if (t->pk_col >= 0) keys.push_back(tup[t->pk_col].i);
            }
            return true;
        });
        for (size_t i = 0; i < to_delete.size(); ++i) {
            RecordId rid = to_delete[i];
            lock_mgr_.acquire(scope.txn, ResourceId{rid.page_id, rid.slot}, LockMode::EXCLUSIVE);
            wal_.append(scope.txn, LogType::DELETE, t->name, rid.page_id, rid.slot, images[i], "");
            hf.erase(rid);
            if (tree) tree->erase(keys[i]);
            t->row_count--;
        }
        catalog_.update(*t);
        ResultSet rs; rs.affected = (int64_t)to_delete.size();
        rs.message = to_string(to_delete.size()) + " row(s) deleted";
        return rs;
    }

    // used by abort_txn to roll back an open txn
    void undo_insert(const LogRecord& r) {
        Page* p = pool_.fetch_page(r.page);
        HeapPage hp(p);
        if (r.slot < hp.header()->num_slots) hp.slots()[r.slot].length = 0;  // tombstone
        pool_.unpin_page(r.page, true);
    }
    void undo_delete(const LogRecord& r) {
        Page* p = pool_.fetch_page(r.page);
        HeapPage hp(p);
        if (r.slot < hp.header()->num_slots) {
            Slot& s = hp.slots()[r.slot];
            if (s.offset > 0 && r.before.size() == (size_t)hp.slots()[r.slot].length)
                memcpy(p->data + s.offset, r.before.data(), r.before.size());
            else
                s.length = (int16_t)r.before.size();  // just re-mark live, offset unchanged
        }
        pool_.unpin_page(r.page, true);
    }

    DiskManager disk_;
    BufferPool pool_;
    Catalog catalog_;
    WAL wal_;
    LockManager lock_mgr_;
    TxnId txn_counter_ = 0;
    TxnId explicit_txn_ = 0;   // 0 = autocommit
    Recovery::Report last_recovery_;
    map<string, unique_ptr<LSMTree>> lsm_;  // one store per lsm table
};

}  // namespace minidb
