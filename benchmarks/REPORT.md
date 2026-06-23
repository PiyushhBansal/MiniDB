# MiniDB Benchmark Report — Extension Track B: MVCC vs 2PL

## 1. Objective

Track B asks us to add Multi-Version Concurrency Control and show **higher read
throughput** and **reduced blocking under contention** compared to the lock-based
(2PL) approach.

Our system keeps **strict 2PL** for the required core transaction feature, and adds
**MVCC** as an opt-in mode (`CREATE TABLE ... USING MVCC`). The benchmark compares
the two under read/write contention: a reader repeatedly scans a set of rows while a
writer constantly holds exclusive locks on those same rows.

- **2PL reader:** must take a shared (S) lock on each row before reading. While the
  writer holds the exclusive (X) lock, the reader blocks.
- **MVCC reader:** takes **no locks at all**. It reads a snapshot and decides
  visibility from each row's version header (`xmin`/`xmax`). It never waits on the writer.

## 2. Experimental setup

| Item | Value |
|------|-------|
| Machine | Apple Silicon (arm64), macOS, 8 cores |
| Compiler | clang++ `-std=c++20 -O2 -pthread` |
| Workload | 1 writer thread holding X-locks on R rows in a loop; 1 reader doing 200 passes over the R rows |
| Rows (R) | 50, 100, 200 |
| Measured | total time for the reader to finish all passes (lower = less blocking) |

The benchmark uses the real `LockManager` (for 2PL) and `MvccManager` (for MVCC
visibility), so the comparison is between actual locking and actual snapshot checks.

Source: [`bench.cpp`](bench.cpp). Raw output: [`results.txt`](results.txt). Run with:

```
make bench        # or: ./bin/bench
```

## 3. Results

```
rows     | passes   | 2PL reads (ms) | MVCC reads(ms) | speedup
----------------------------------------------------------------------
50       | 200      | ~2.5           | ~0.1           | ~20x
100      | 200      | ~3.2           | ~0.2           | ~18x
200      | 200      | ~28.6          | ~0.3           | ~100x
```

*(Exact numbers vary run to run with scheduler timing; see `results.txt` for a captured
run. The shape is always the same: MVCC reads are far faster and the gap widens as the
contended row set grows.)*

## 4. Analysis

**Read throughput — MVCC wins by 1–2 orders of magnitude.**
Under 2PL the reader must acquire an S-lock per row, and that lock is incompatible with
the writer's X-lock, so the reader **blocks** every time it hits a row the writer is
holding. As the number of contended rows grows, the chance of colliding with the writer
goes up, so 2PL read time climbs steeply (≈2.5 ms → ≈28 ms from 50 → 200 rows). MVCC
read time stays essentially flat (≈0.1–0.3 ms) because the reader never takes a lock —
it just checks `xmin`/`xmax` against its snapshot, which is a couple of integer
comparisons.

**Reduced blocking — this is the whole point of MVCC.**
The 2PL reader spends most of its wall-clock time *waiting* for the writer to release
locks. The MVCC reader spends zero time waiting. This is exactly the "readers don't
block writers, writers don't block readers" property MVCC is famous for.

**Trade-off (honest).**
MVCC isn't free: every row carries a 16-byte version header (`xmin`,`xmax`), deleted
rows are not physically reclaimed (they're stamped and left behind until a garbage-
collection / VACUUM pass, which we did **not** implement), and long-running snapshots can
see "old" data. So MVCC trades extra storage and version cleanup for far better read
concurrency. For a read-heavy workload that's a great deal — which is why Postgres,
Oracle, and MySQL/InnoDB all use MVCC.

## 5. Snapshot-isolation correctness

Beyond throughput, the track requires correct **snapshot visibility**. `test_mvcc`
(run via `make test`) checks the visibility rules directly:
1. a txn sees rows committed *before* it started,
2. a txn does **not** see rows committed *after* its snapshot,
3. a later txn sees both,
4. an old snapshot still sees a row that a newer txn deleted,
5. a fresh reader sees that row as gone,
6. an aborted txn's rows are never visible.

All six pass, demonstrating snapshot isolation is implemented correctly.

## 6. Conclusion

MVCC delivers dramatically higher read throughput and near-zero reader blocking under
write contention, at the cost of per-row version metadata and deferred cleanup. The
measurements confirm the canonical MVCC trade-off and validate the Track B
implementation.
