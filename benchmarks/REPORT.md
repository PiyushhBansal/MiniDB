# MiniDB Benchmark Report — Extension Track C: LSM-tree vs B+ tree storage

## 1. Objective

Track C asks us to replace heap-file storage with an LSM-tree design and compare
it against the B+ tree-based storage on **write throughput**, **read latency**,
and **storage amplification**. This report presents that comparison.

Both engines store the identical workload — `N` rows of `(int64 key → value)` —
through MiniDB's own components:

- **B+ tree path:** tuples in a slotted **heap file** + a persistent **B+ tree**
  index on the primary key (MiniDB's default row store).
- **LSM path:** the log-structured engine (`MemTable → SSTable → compaction`).

## 2. Experimental setup

| Item | Value |
|------|-------|
| Machine | Apple Silicon (arm64), macOS, 8 cores |
| Compiler | clang++ `-std=c++20 -O2` |
| Buffer pool | 8192 frames (32 MiB) so the B+ tree working set is resident |
| LSM MemTable limit | 4096 keys before flush |
| LSM compaction threshold | 4 SSTables |
| Value size | 32 bytes and 256 bytes (two runs) |
| Row counts | 10K, 50K, 100K, 200K |
| Read phase | up to 50,000 uniformly-random point lookups |

Throughput is `rows / write_time`. Read latency is `read_time / lookups` in µs.
Disk bytes is the on-disk size of the data file(s) after the write phase.

The benchmark source is [`bench.cpp`](bench.cpp); raw output is in
[`results.txt`](results.txt). Reproduce with:

```
clang++ -std=c++20 -O2 bench.cpp -o bench && ./bench 32
```

## 3. Results (value ≈ 32 bytes)

| Rows | Write tput (ins/s) B+ / LSM | Read latency (µs) B+ / LSM | Disk bytes B+ / LSM |
|-----:|-----------------------------|----------------------------|---------------------|
| 10K  | 1,880 / 101,670 (**54×**)   | 5.6 / 29.1 (B+ **5.2×**)   | 807K / 610K (0.76×) |
| 50K  | 1,839 / 47,782 (**26×**)    | 7.2 / 28.1 (B+ **3.9×**)   | 4.0M / 3.1M (0.76×) |
| 100K | 1,299 / 13,694 (**10×**)    | 17.1 / 58.4 (B+ **3.4×**)  | 8.0M / 6.1M (0.76×) |
| 200K | 1,874 / 17,590 (**9×**)     | 6.1 / 27.9 (B+ **4.6×**)   | 16.1M / 12.2M (0.76×)|

*(See `results.txt` for the 256-byte-value run, which shows the same shape.)*

## 4. Analysis

**Write throughput — LSM wins by 9–54×, exactly as theory predicts.**
The LSM buffers every write in an in-memory sorted `MemTable` and only touches
disk in large *sequential* batches when it flushes an SSTable. The B+ tree
instead does, per insert: a heap-page write **plus** a root-to-leaf descent and
in-place index update, occasionally splitting nodes — all random-access work.
LSM's advantage shrinks as `N` grows because compaction does more merge work,
which is the expected write-amplification cost of the design.

**Read latency — the B+ tree wins by 3–5×, again as expected.**
A B+ tree point lookup is a single logarithmic descent to exactly one leaf. The
LSM must consult the MemTable and then potentially several SSTables newest-first
until it finds the key — classic **read amplification**. A production LSM hides
much of this with Bloom filters and block caches (a documented future
improvement); even without them the gap is a modest constant factor.

**Storage — LSM uses ~24% less space.**
SSTables pack entries sequentially with no per-page free space, whereas the heap
file's slotted pages carry headers, a slot array, and unfilled tail space
(internal fragmentation). After major compaction the LSM also physically drops
overwritten/deleted keys.

## 5. Why the absolute B+ tree write number is modest

Our B+ tree deliberately deserializes a whole node into C++ vectors on each
`load`, mutates it, and reserializes on `store` (see `bplus_tree.h` header
comment). This was a conscious **clarity-over-speed** choice for a teaching
system that has to be defended in a viva — it makes splits trivial to read. An
in-place byte-level node mutation would raise the absolute B+ tree throughput,
but it would **not** change the qualitative result: the LSM is write-optimised
and the B+ tree is read-optimised. The relative comparison is what Track C asks
for, and it holds.

## 6. Conclusion

The measurements confirm the canonical LSM trade-off and validate the Track C
implementation: **the LSM engine trades read latency and a little extra read
work for dramatically higher write throughput and lower storage footprint**,
making it the right engine for write-heavy workloads — which is precisely why
LevelDB/RocksDB/Cassandra are built on it.
