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
#include "mvcc.h"
#include <iostream>
#include <sstream>
#include <map>

using namespace std;

namespace minidb {

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
        // index pages aren't WAL'd, so the B+tree may be stale after recovery.
        // rebuild each heap table's PK index from the now-correct heap.
        rebuild_indexes();
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
        mvcc_.begin(t);            // mark active + take a snapshot for mvcc tables
        return t;
    }
    void commit_txn(TxnId t) {
        wal_.append(t, LogType::COMMIT);
        wal_.flush();              // only flush the log here, not the data pages.
        // durability comes from the wal: even if dirty pages are still in the buffer
        // pool at crash, redo replays them from the log. one sequential log flush
        // instead of random page writes. pages get written lazily later.
        mvcc_.commit(t);           // mvcc: now this txn's versions become visible to others
        lock_mgr_.release_all(t);  // strict 2PL: release only now, at commit
    }

    // clean shutdown calls this, crash skips it so recovery rebuilds from the wal.
    void checkpoint() {
        pool_.flush_all();
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
            else if (it->type == LogType::UPDATE) undo_update(*it);
        }
        wal_.append(t, LogType::ABORT);
        wal_.flush();
        pool_.flush_all();
        mvcc_.abort(t);            // mvcc: aborted txn's versions stay invisible
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
    // Rebuild each heap table's PK index from its recovered heap. The WAL logs
    // heap slot changes but not index pages, so after a crash the B+tree can be
    // out of sync with the heap. We build a fresh tree (old pages are leaked -
    // MiniDB never reclaims pages anyway) and also recount live rows so the
    // optimizer's cost model is accurate post-recovery. MVCC tables read via the
    // snapshot scan, not the index, so they're skipped.
    void rebuild_indexes() {
        for (const string& name : catalog_.table_names()) {
            TableInfo* t = catalog_.get(name);
            if (!t || t->pk_col < 0 || t->storage == StorageKind::MVCC) continue;
            BPlusTree tree(&pool_, BPlusTree::create(&pool_));
            HeapFile hf(&pool_, t->heap_root);
            int64_t live = 0;
            hf.scan([&](RecordId rid, const string& rec) {
                Tuple tup = deserialize_tuple(rec.data(), rec.size(), t->schema);
                tree.insert(tup[t->pk_col].i, rid);
                live++;
                return true;
            });
            t->index_root = tree.root();
            t->row_count = live;
            catalog_.update(*t);
        }
        pool_.flush_all();
    }

    ResultSet exec_create(const CreateStmt& c) {
        if (catalog_.has_table(c.table)) throw DBError("table already exists: " + c.table);
        TableInfo t;
        t.name = c.table;
        t.schema = c.schema;
        t.pk_col = c.pk_column.empty() ? -1 : c.schema.index_of(c.pk_column);
        t.row_count = 0;
        t.storage = c.use_mvcc ? StorageKind::MVCC : StorageKind::HEAP;
        // both heap and mvcc use the same heap-file storage. mvcc just stores a
        // version header (xmin/xmax) in front of each tuple and skips read locks.
        t.heap_root = HeapFile::create(&pool_);
        if (t.pk_col >= 0) t.index_root = BPlusTree::create(&pool_);
        catalog_.put(t);
        pool_.flush_all();
        ResultSet rs; rs.message = "table '" + c.table + "' created" +
                                   (c.use_mvcc ? " (MVCC)" : "");
        return rs;
    }

    // write row to heap, add to pk index, log it. note the ordering: wal record
    // is appended before the page ever reaches disk - that's the write-ahead rule.
    // X-lock the row and hold it to commit (strict 2PL).
    ResultSet exec_insert(const InsertStmt& ins) {
        TableInfo* t = catalog_.get(ins.table);
        if (!t) throw DBError("no such table: " + ins.table);

        auto scope = txn_scope();
        HeapFile hf(&pool_, t->heap_root);
        bool mvcc = t->storage == StorageKind::MVCC;
        int64_t n = 0;
        for (const auto& row : ins.rows) {
            if (row.size() != t->schema.size())
                throw DBError("column count mismatch on INSERT");
            // PRIMARY KEY uniqueness. for heap tables the index tracks live keys
            // exactly (DELETE erases the key), so a hit means a duplicate. MVCC
            // keeps deleted versions in place, so we don't index-check those here.
            if (t->pk_col >= 0 && !mvcc) {
                BPlusTree idx(&pool_, t->index_root);
                if (idx.search(row[t->pk_col].i))
                    throw DBError("duplicate primary key: " + row[t->pk_col].to_string());
            }
            // mvcc: stamp xmin = this txn, xmax = 0 (live). heap: plain bytes.
            string image = mvcc ? serialize_versioned(row, t->schema, scope.txn, 0)
                                : serialize_tuple(row, t->schema);
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
        if (t->storage == StorageKind::MVCC) {
            // mvcc: scan the heap but only show versions visible to our snapshot.
            // no read lock taken, so readers never block writers.
            Snapshot snap = current_snapshot();
            driver = make_unique<MvccScan>(&pool_, *t, &mvcc_, snap);
            if (explain_out) *explain_out += "  " + s.table + ": MVCC scan (snapshot " +
                                             to_string(snap.xid) + ")\n";
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

        // mvcc delete: don't remove the row, just stamp xmax = this txn on the
        // visible version. old readers with an earlier snapshot still see it.
        if (t->storage == StorageKind::MVCC) {
            auto scope = txn_scope();
            Snapshot snap = mvcc_.snapshot_for(scope.txn);
            HeapFile hf(&pool_, t->heap_root);
            int64_t n = 0;
            hf.scan([&](RecordId rid, const string& rec) {
                VersionHeader vh = read_version_header(rec.data());
                if (!mvcc_.visible(vh, snap)) return true;     // not our version, skip
                Tuple tup = deserialize_tuple(rec.data() + VHDR_SIZE, rec.size() - VHDR_SIZE, t->schema);
                Row row{tup, rid};
                for (auto& p : d.where) if (!eval_pred(p, row, t->schema)) return true;
                // stamp xmax in place (same length, just overwrite the 8 bytes)
                lock_mgr_.acquire(scope.txn, ResourceId{rid.page_id, rid.slot}, LockMode::EXCLUSIVE);
                string before = rec;
                string after = rec; set_xmax(after, scope.txn);
                wal_.append(scope.txn, LogType::UPDATE, t->name, rid.page_id, rid.slot, before, after);
                write_version_inplace(rid, after);
                t->row_count--; n++;
                return true;
            });
            catalog_.update(*t);
            ResultSet rs; rs.affected = n; rs.message = to_string(n) + " row(s) deleted";
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
    // mvcc delete logs an UPDATE (xmax change). undo = put the before-image back.
    void undo_update(const LogRecord& r) {
        Page* p = pool_.fetch_page(r.page);
        HeapPage hp(p);
        if (r.slot < hp.header()->num_slots) {
            Slot& s = hp.slots()[r.slot];
            if (s.length > 0 && r.before.size() == (size_t)s.length)
                memcpy(p->data + s.offset, r.before.data(), r.before.size());
        }
        pool_.unpin_page(r.page, true);
    }

    // overwrite a slot's bytes in place (mvcc xmax stamp - same length so safe).
    void write_version_inplace(RecordId rid, const string& image) {
        Page* p = pool_.fetch_page(rid.page_id);
        HeapPage hp(p);
        Slot& s = hp.slots()[rid.slot];
        if ((size_t)s.length == image.size())
            memcpy(p->data + s.offset, image.data(), image.size());
        pool_.unpin_page(rid.page_id, true);
    }

    // snapshot for the current statement. inside a BEGIN block we reuse the txn's
    // snapshot; an autocommit SELECT gets a fresh one-shot snapshot.
    Snapshot current_snapshot() {
        if (explicit_txn_) return mvcc_.snapshot_for(explicit_txn_);
        return mvcc_.fresh_readonly_snapshot();
    }

    DiskManager disk_;
    BufferPool pool_;
    Catalog catalog_;
    WAL wal_;
    LockManager lock_mgr_;
    MvccManager mvcc_;
    TxnId txn_counter_ = 0;
    TxnId explicit_txn_ = 0;   // 0 = autocommit
    Recovery::Report last_recovery_;
};

}  // namespace minidb
