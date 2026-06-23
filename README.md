# MiniDB — A Working Relational Database Engine

> Advanced DBMS Capstone Project. MiniDB is a small but complete relational
> database engine written from scratch in C++20: page-based storage with a
> buffer pool, a B+ tree index, a SQL parser and Volcano-model executor, a
> cost-based optimizer, serializable transactions via strict two-phase locking,
> write-ahead logging with crash recovery, and — as the chosen extension —
> opt-in **MVCC** (multi-version concurrency control) so readers never block on
> writers.

---

## Team

| Full Name | Roll Number | Scaler Email |
|-----------|-------------|--------------|
| _<member 1>_ | _SCALERxxxxx_ | _xxx@scaler.com_ |
| _<member 2>_ | _SCALERxxxxx_ | _xxx@scaler.com_ |
| _<member 3>_ | _SCALERxxxxx_ | _xxx@scaler.com_ |
| _<member 4>_ | _SCALERxxxxx_ | _xxx@scaler.com_ |

> **Team name:** `Team_MiniDB` &nbsp;•&nbsp; **Extension track:** **B — Concurrency (MVCC)**
> _(Fill in the member details above before submitting the PR.)_

---

## 1. Project Overview

**Problem statement.** Build a functioning relational database from foundational
components and integrate them into one coherent system, demonstrating real
database internals rather than a thin wrapper over existing tooling.

**Goals.**
- A real on-disk **storage engine** (pages, buffer pool with LRU eviction, heap files).
- A persistent **B+ tree** index used by query execution.
- An end-to-end **SQL** path: parse → plan → execute for `SELECT` (with `WHERE`
  and `JOIN`), `INSERT`, `DELETE`.
- A **cost-based optimizer** that chooses index vs sequential scan and join order.
- **Serializable** transactions with strict **2PL** and deadlock detection.
- **WAL** + **crash recovery** that preserves committed transactions.
- **Extension Track B:** opt-in **MVCC**, benchmarked against plain 2PL under contention.

**Chosen extension track:** **B — Concurrency (MVCC).** We keep strict 2PL +
deadlock detection as the required core, and add MVCC as an opt-in mode per table
with `CREATE TABLE ... USING MVCC`. MVCC tables reuse the same heap-file storage
but prepend a 16-byte version header (`xmin`/`xmax`) to every tuple; each
transaction takes a snapshot at begin, and reads emit only the versions visible
to that snapshot — so readers take no locks and never block writers. See
[§9](#9-extension-track-b--mvcc-concurrency) and
[`benchmarks/REPORT.md`](benchmarks/REPORT.md).

---

## 2. System Architecture

```
                         ┌──────────────────────────────┐
                         │           SQL shell           │   src/main.cpp
                         │      (REPL, table printer)     │
                         └───────────────┬────────────────┘
                                         │ SQL text
                         ┌───────────────▼────────────────┐
                         │            Parser               │   parser.h, ast.h
                         │  tokenizer + recursive descent  │
                         └───────────────┬────────────────┘
                                         │ Statement (AST)
                         ┌───────────────▼────────────────┐
        ┌────────────────┤        Database engine          │   database.h
        │                │  planning, DDL/DML, txn control │
        │                └──────┬───────────────┬──────────┘
        │                       │               │
        │            ┌──────────▼──┐   ┌────────▼─────────┐
        │  optimizer │  Optimizer  │   │   Executor       │   executor.h
        │ (cost model│ access path │   │ Volcano operators│  SeqScan/IndexScan/
        │  selectvty)│ join order  │   │ Filter/HashJoin/ │  MvccScan/Filter
        │            └─────────────┘   │ scan/join/filter │
        │   ┌──────────────┐  ┌────────────▼───┐ ┌───▼──────────────┐
        │   │ LockManager  │  │   B+ Tree      │ │   MvccManager    │  bplus_tree.h
        │   │ strict 2PL   │  │   (PK index)   │ │ version headers  │  mvcc.h
        │   │ deadlock det │  └───────┬────────┘ │ snapshot visibty │
        │   └──────────────┘          │          └───────┬──────────┘
        │   ┌──────────────┐  ┌───────▼────────┐         │ visibility check
        └──►│  WAL + Recov │  │   Heap File    │◄────────┘ (MVCC tables
            │ redo/undo    │  │ slotted pages  │  heap_file.h  reuse heap)
            └──────┬───────┘  └───────┬────────┘
                   │                  │
            ┌──────▼──────────────────▼────────┐
            │          Buffer Pool              │   buffer_pool.h
            │   frames + pin/dirty + LRU        │
            └──────────────┬────────────────────┘
                           │ page reads/writes
            ┌──────────────▼────────────────────┐
            │          Disk Manager              │   disk_manager.h
            │   fixed 4 KiB pages in one file     │
            └────────────────────────────────────┘
```

**Major modules** (all in [`src/`](src/), header-only engine + one `main.cpp`):

| File | Responsibility |
|------|----------------|
| `common.h` | shared types: `PageId`, `RecordId`, `TxnId`, `Lsn`, constants |
| `disk_manager.h` | raw 4 KiB page I/O against one file |
| `buffer_pool.h` | page cache, pin/unpin, dirty tracking, LRU eviction |
| `tuple.h` | `Value`/`Schema`/`Tuple` and tuple (de)serialization |
| `heap_file.h` | slotted-page heap storage + sequential scan |
| `bplus_tree.h` | persistent B+ tree (search / insert+split / delete / range) |
| `catalog.h` | data dictionary (tables, schemas, roots, storage kind) |
| `parser.h`, `ast.h` | SQL tokenizer + recursive-descent parser → AST |
| `optimizer.h` | selectivity, cost model, access-path & join-order choice |
| `executor.h` | Volcano operators (scan/filter/join) |
| `lock_manager.h` | strict 2PL, S/X locks, wait-for-graph deadlock detection |
| `wal.h` | write-ahead log records + base64 image encoding |
| `recovery.h` | ARIES-inspired analysis / redo / undo |
| `mvcc.h` | **Track B** — `MvccManager`: txn states, snapshots, version visibility |
| `database.h` | the glue: planning, DDL/DML, transactions, recovery |

**Data flow for a query.** SQL text → `Parser` → `Statement` → `Database`
builds an operator tree (consulting the `Optimizer`) → operators pull tuples
from `HeapFile`/`BPlusTree` through the `BufferPool` → results materialized and
printed. On MVCC tables the `MvccScan` operator filters versions through the
`MvccManager`'s snapshot-visibility check. Writes additionally go through the
`LockManager` (2PL) and `WAL` (durability).

---

## 3. Storage Layer

**Page format.** The database is a single file of fixed **4 KiB pages**
(`PAGE_SIZE`). The `DiskManager` maps page id `N` to byte offset `N × 4096`;
new pages are allocated by extending the file.

**Heap files (slotted pages).** A table's rows live in a chain of heap pages.
Each page uses a *slotted* layout to support variable-length records and stable
addressing:

```
+--------------------------------------------------------------+
| PageHeader | slot[0] slot[1] … →        ← … record1 record0  |
+--------------------------------------------------------------+
  next_page                ↑ slot array       ↑ records grow
  num_slots                  grows down          downward (toward slots)
  free_ptr
```

- A **slot** is `(offset, length)`; `length == 0` is a tombstone (deleted),
  so a `RecordId = (page_id, slot)` stays valid forever — important because the
  B+ tree stores `RecordId`s.
- Inserts append from the tail; a `last_page_` hint makes a run of inserts
  amortized O(1) instead of O(chain length).

**Buffer pool.** A fixed array of in-memory **frames**. `fetch_page` pins a page
(loading from disk on miss); `unpin_page` releases it and marks it dirty if
modified. When full, the **least-recently-used unpinned** frame is evicted
(written back if dirty). This is the textbook *pin-count + dirty-bit + LRU*
design; see `buffer_pool.h`.

---

## 4. Indexing

**B+ tree** (`bplus_tree.h`), keyed on `int64` primary keys, mapping each key to
the tuple's `RecordId`.

- **Node structure.** Each node is one buffer-pool page with a header
  (`is_leaf`, `num_keys`, `next_leaf`). Leaves store `key → RecordId` and a
  right-sibling pointer (a linked list enabling **range scans**); internal nodes
  store `keys` separating `children` pointers. Node fan-out is derived from
  `PAGE_SIZE` (`LEAF_MAX = 255`, `INTERNAL_MAX = 340`).
- **Search path.** Descend from the root, at each internal node following the
  child for the first separator `> key`, until a leaf; binary-search the leaf.
- **Insert** splits a full leaf/internal node and propagates the split key
  upward, growing the tree by replacing the root when it splits.
- **Delete** removes the key from its leaf (lazy; underflow is not rebalanced —
  see [Limitations](#11-limitations)).

The index is used automatically by query execution when the optimizer picks an
index scan (see §6).

---

## 5. Query Execution

**Parser.** A hand-written tokenizer + recursive-descent parser (`parser.h`)
produces a `Statement` AST (`ast.h`). Supported grammar:

```
CREATE TABLE t (col TYPE, …, PRIMARY KEY (col)) [USING MVCC|HEAP];
INSERT INTO t VALUES (…), (…);
SELECT a, b | *
       FROM t [JOIN u ON t.x = u.y]
       [WHERE p AND p …];
DELETE FROM t [WHERE …];
EXPLAIN SELECT …;
BEGIN; COMMIT; ROLLBACK;
```

**Query-plan generation.** `Database::plan_select` turns a `SelectStmt` into a
tree of physical operators, consulting the optimizer for the driving table's
access path and the join order.

**Operator execution (Volcano / iterator model).** Every operator exposes
`open() / next() / close()` and pulls tuples from its children one at a time:

| Operator | Purpose |
|----------|---------|
| `SeqScan` | full heap-file scan |
| `IndexScan` | B+ tree range probe → heap fetch (chosen by optimizer) |
| `MvccScan` | scan the heap, emit only versions visible to the snapshot (no read locks) |
| `Filter` | apply ANDed `WHERE` predicates |
| `HashJoin` | inner equi-join; builds a hash table on the smaller side |

`EXPLAIN` prints the chosen plan (access path + join build side).

---

## 6. Optimizer

A System-R-style **cost-based** optimizer (`optimizer.h`). Scope: per-table
**access-path selection** (index vs sequential scan) and **two-table join
ordering** (which side to build the hash table on). It does *not* do
N-table join enumeration (Selinger-style dynamic programming) or histograms —
selectivities use the textbook System-R constants below.

- **Selectivity estimation.** Equality on a unique PK → ≈ `1/row_count` (one
  row); range predicates → the textbook `1/3`; `≠` → `1 − 1/n`. Conjunctions
  multiply selectivities.
- **Cost estimation.** Sequential scan ≈ `ceil(rows / tuples_per_page)` page
  accesses. Index scan ≈ `log₂(n)` descent + one heap access per matching row.
- **Access-path choice.** If a `WHERE` predicate is an equality/range on the
  indexed PK, the optimizer compares index-scan vs seq-scan cost and uses the
  cheaper one. On small tables seq-scan correctly wins; on large tables the
  index wins — demonstrable with `EXPLAIN`:

  ```
  minidb> EXPLAIN SELECT * FROM t WHERE id = 1500;   -- 2000-row table
  QUERY PLAN:
    t: index scan on PK 'id' (cost 11.97 < seq 32.00)
  ```

- **Join ordering.** For a two-table join the optimizer estimates each side's
  output cardinality and builds the hash table on the **smaller** side, probing
  with the larger — minimizing the in-memory hash table.

---

## 7. Transactions & Concurrency

- **Isolation:** **Serializable**, via **strict two-phase locking**. All locks a
  transaction acquires are held until commit/abort, which guarantees both
  serializability and recoverability (no cascading aborts).
- **Locking strategy:** **row-level** Shared/Exclusive locks keyed by
  `(page, slot)` in `LockManager` (`lock_manager.h`). Reads take S, writes take
  X; S is compatible only with S. Re-entrancy and S→X upgrade are handled.
- **Deadlock handling:** a **wait-for graph** is maintained as transactions
  block; before sleeping on a lock, the manager runs DFS cycle detection. If a
  request would close a cycle, the requester is chosen as the **victim** and
  throws `DeadlockAbort`, which the engine catches to roll the transaction back
  and release its locks.
- **Modes:** statements run in **autocommit** by default; `BEGIN … COMMIT/
  ROLLBACK` groups multiple statements into one transaction.

`make test` exercises S/S compatibility, X blocking, and a real two-transaction
deadlock (`src/test_locking.cpp`), plus the six snapshot-visibility properties of
the MVCC mode (`src/test_mvcc.cpp` — see [§9](#9-extension-track-b--mvcc-concurrency)).

---

## 8. Recovery

**WAL design** (`wal.h`). An append-only log; every data change is logged with a
physical **before-image** and **after-image** for its `(page, slot)`, tagged
with the transaction id, *before* the change is allowed to reach the data file.
Records are human-readable (one per line) for transparency:

```
<lsn>|<txn>|<type>|<table>|<page>|<slot>|<before_b64>|<after_b64>
types: BEGIN, INSERT, DELETE, UPDATE, COMMIT, ABORT, CHECKPOINT
```

**Durability via the log.** On `COMMIT` the WAL is force-flushed; dirty *data*
pages are **not** forced (that is the whole point of WAL — turn random page
flushes into one sequential log flush). Pages are written lazily; recovery
replays anything that didn't make it.

**Crash-recovery procedure** (`recovery.h`, ARIES-*inspired*, run at every startup):
1. **Analysis** — scan the log to find which transactions committed.
2. **Redo** — re-apply *all* logged changes in order (repeating history) to
   rebuild the page state at crash time.
3. **Undo** — roll back the "loser" transactions (started, never committed) in
   reverse using before-images.

> We follow ARIES's three-pass *repeating-history* structure, but keep it
> simplified: redo is idempotent because records hold full physical before/after
> images (rather than via per-page LSN comparison), and undo writes plain
> before-images rather than ARIES Compensation Log Records (CLRs).

Net guarantee: **committed transactions survive a crash; uncommitted ones
vanish.** Demonstrable in the shell with `\crash` (exit without flushing pages):

```
-- session 1
INSERT INTO t VALUES (1,'committed-A');     -- autocommit → durable
BEGIN; INSERT INTO t VALUES (99,'uncommitted'); \crash
-- session 2 (restart)
recovery: 2 committed txn(s) redone, 1 loser(s) rolled back …
SELECT * FROM t;   -- shows 1 (and 2), NOT 99
```

---

## 9. Extension Track B — MVCC (Concurrency)

**Motivation.** Under plain strict 2PL, a reader takes an S-lock on every row it
touches, so the moment a writer holds an X-lock the reader has to wait — readers
block on writers and vice-versa. MVCC fixes this for reads: instead of locking,
each transaction reads from a consistent **snapshot** of the database, so readers
see a stable view and never block, while writers still serialize among themselves.

**Design** (`mvcc.h`, `tuple.h`):
- **Version header.** MVCC tables reuse the normal heap-file storage, but every
  stored tuple is prefixed with a 16-byte header:
  `[xmin (8B)][xmax (8B)][column bytes…]`. `xmin` is the txn that created the
  version; `xmax` is the txn that deleted it (`0` = still live). See
  `VersionHeader` / `serialize_versioned` / `read_version_header` / `set_xmax` /
  `VHDR_SIZE` in `tuple.h`.
- **MvccManager** tracks each transaction's state (ACTIVE / COMMITTED / ABORTED),
  and at `begin()` takes a **Snapshot**: the set of txns active at that moment
  plus a `next` boundary id. Commit/abort notify the manager.
- **Visibility rule** — `visible(VersionHeader, Snapshot)`: a version is visible
  iff its `xmin` is committed-in-our-view (our own txn, OR committed AND
  `id < snapshot.next` AND not in the snapshot's active set) AND its `xmax` is
  *not* committed-in-our-view (i.e. as far as we can see it has not been deleted).
- **INSERT** on an MVCC table (`exec_insert` in `database.h`) writes a versioned
  tuple with `xmin = current txn`, `xmax = 0`.
- **DELETE** (`exec_delete`) does **not** remove the row; it stamps
  `xmax = current txn` on the visible version in place and logs it as a WAL
  `UPDATE`. Older snapshots still see the row.
- **MvccScan** (`executor.h`) scans the heap, reads each version header, and emits
  only the rows visible to the current snapshot — taking **no read locks**, which
  is why readers don't block writers. `EXPLAIN` prints `MVCC scan (snapshot N)`.
- **Locks.** Readers take no locks; writers still take X-locks for write-write
  protection.

**Results** (full analysis in [`benchmarks/REPORT.md`](benchmarks/REPORT.md);
`benchmarks/bench.cpp` runs MVCC lock-free reads vs 2PL S-lock reads under a
concurrent writer):

| Metric | Outcome |
|--------|---------|
| Read throughput under contention | **MVCC ~8–100× faster** than 2PL |
| Reader blocking | **MVCC readers never block** on the writer; 2PL readers stall on X-locks |
| Cost | **+16 bytes/row** version header, and no version GC (old/deleted versions accumulate) |

This is the canonical MVCC trade-off: lock-free, snapshot-consistent reads in
exchange for per-row version overhead.

---

## 10. Benchmarks

- **Setup:** Apple Silicon, clang `-O2`; concurrent point-read workload while a
  background writer mutates the table — MVCC (lock-free snapshot reads) vs 2PL
  (S-lock reads) measuring read throughput and reader stalls.
- **Code & raw data:** [`benchmarks/bench.cpp`](benchmarks/bench.cpp),
  [`benchmarks/results.txt`](benchmarks/results.txt).
- **Full analysis:** [`benchmarks/REPORT.md`](benchmarks/REPORT.md).

Reproduce:
```
make bench          # MVCC vs 2PL under a concurrent writer
```

---

## 11. Limitations

- **B+ tree delete** is lazy: keys are removed from leaves but underflowing nodes
  are not merged/redistributed, so a delete-heavy workload can leave sparse
  nodes. Search/scan remain correct.
- **One secondary index path is not exposed** — indexes are over the primary key
  only (the spec lists a secondary index as optional).
- **Catalog is a text sidecar** and is not WAL-protected (DDL is not crash-atomic);
  data DML *is* logged and recovered.
- **MVCC has no version garbage collection / `VACUUM`:** deleted versions (those
  with `xmax` set) and superseded versions are never reclaimed, so MVCC tables
  grow monotonically over a session.
- **MVCC snapshots are simple:** visibility is snapshot-isolation only — there is
  no true serializable-snapshot-isolation read/write conflict detection beyond the
  writers' X-locks.
- **MVCC version header costs 16 bytes/row** on top of the tuple itself.
- **Single-file, single-node**; no networking; numeric (`int64`) primary keys.
- **B+ tree node (de)serialization** favors readability over raw speed (see
  `bplus_tree.h`); this lowers absolute B+ tree write throughput but not the
  qualitative benchmark conclusions.

**Future improvements:** node merging on delete; secondary indexes; version GC /
`VACUUM` for MVCC tables; serializable-snapshot conflict detection; group-commit.

---

## 12. How to Run

**Dependencies:** a C++20 compiler (clang ≥ 14 / g++ ≥ 11). No third-party
libraries. `pthread` for the locking test. CMake optional.

**Build (Makefile — one command):**
```
make            # builds bin/minidb, bin/bench, and the tests
```

**Build (CMake):**
```
cmake -S . -B build && cmake --build build
ctest --test-dir build      # run component tests
```

**Run the shell:**
```
./bin/minidb mydb.db        # or: make run
```

**Run the tests / benchmark:**
```
make test                   # storage, B+ tree, mvcc, locking
make bench                  # MVCC vs 2PL
```

**Example session:**
```sql
CREATE TABLE emp (id INT, name VARCHAR, dept INT, PRIMARY KEY (id));
CREATE TABLE dept (id INT, dname VARCHAR, PRIMARY KEY (id));
INSERT INTO dept VALUES (10,'Eng'),(20,'Sales');
INSERT INTO emp  VALUES (1,'alice',10),(2,'bob',20),(3,'carol',10);

SELECT emp.name, dept.dname FROM emp JOIN dept ON emp.dept = dept.id
       WHERE emp.dept = 10;

EXPLAIN SELECT * FROM emp WHERE id = 2;     -- show the chosen plan

BEGIN; INSERT INTO emp VALUES (4,'dave',20); ROLLBACK;   -- rolled back

CREATE TABLE acct (id INT, name VARCHAR, bal INT, PRIMARY KEY (id)) USING MVCC;  -- Track B
INSERT INTO acct VALUES (1,'alice',100),(2,'bob',200);
DELETE FROM acct WHERE id = 2;   -- stamps xmax, doesn't physically remove
SELECT * FROM acct;              -- bob no longer visible to new snapshots
```

Shell meta-commands: `\tables`, `\checkpoint`, `\crash` (simulate power loss),
`\q` (clean shutdown), `\help`.

A ready-to-pipe demo lives at [`docs/demo.sql`](docs/demo.sql):
```
./bin/minidb demo.db < docs/demo.sql
```
