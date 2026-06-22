// LSM vs B+tree benchmark. same workload both engines, write phase then random reads.
#include "../src/bplus_tree.h"
#include "../src/heap_file.h"
#include "../src/lsm_tree.h"
#include <chrono>
#include <random>
#include <cstdio>
#include <filesystem>

using namespace std;
using namespace minidb;
using Clock = chrono::steady_clock;
namespace fs = filesystem;

static double secs(Clock::time_point a, Clock::time_point b) {
    return chrono::duration<double>(b - a).count();
}

// total bytes on disk (storage metric)
static int64_t dir_bytes(const string& path) {
    int64_t total = 0;
    if (fs::is_regular_file(path)) return (int64_t)fs::file_size(path);
    if (!fs::exists(path)) return 0;
    for (auto& e : fs::recursive_directory_iterator(path))
        if (e.is_regular_file()) total += (int64_t)e.file_size();
    return total;
}

struct Result { double write_tput; double read_us; int64_t bytes; };

// pad out a value to ~vsize bytes, same for a given key every time
static string make_value(int64_t k, int vsize) {
    string s = "v" + to_string(k);
    s.resize(max<size_t>(s.size(), vsize), 'x');
    return s;
}

Result bench_btree(int N, int vsize) {
    string dbf = "/tmp/bench_btree.db";
    remove(dbf.c_str());
    DiskManager dm(dbf);
    // big buffer pool so we measure the engine not eviction thrashing
    BufferPool bp(&dm, 8192);
    Schema sch{{{"id", Type::INT}, {"v", Type::VARCHAR}}};
    PageId heap = HeapFile::create(&bp);
    PageId idx = BPlusTree::create(&bp);
    HeapFile hf(&bp, heap);
    BPlusTree tree(&bp, idx);

    // write phase: insert N tuples + index entries (write throughput)
    auto t0 = Clock::now();
    for (int64_t k = 0; k < N; ++k) {
        Tuple t{Value::Int(k), Value::Str(make_value(k, vsize))};
        RecordId rid = hf.insert(serialize_tuple(t, sch));
        tree.insert(k, rid);
    }
    bp.flush_all();
    auto t1 = Clock::now();

    // read phase: M random point lookups via the index (read latency)
    mt19937_64 rng(123);
    int M = min(N, 50000);
    auto r0 = Clock::now();
    volatile int64_t sink = 0;
    for (int i = 0; i < M; ++i) {
        int64_t k = rng() % N;
        auto rid = tree.search(k);
        if (rid) { auto rec = hf.get(*rid); if (rec) sink += rec->size(); }
    }
    auto r1 = Clock::now();
    (void)sink;

    Result res;
    res.write_tput = N / secs(t0, t1);
    res.read_us = secs(r0, r1) / M * 1e6;
    res.bytes = dir_bytes(dbf);
    return res;
}

Result bench_lsm(int N, int vsize) {
    string dir = "/tmp/bench_lsm";
    fs::remove_all(dir);
    fs::create_directories(dir);
    LSMTree lsm(dir, /*memtable_limit=*/4096, /*compaction_threshold=*/4);
    Schema sch{{{"id", Type::INT}, {"v", Type::VARCHAR}}};

    auto t0 = Clock::now();
    for (int64_t k = 0; k < N; ++k) {
        Tuple t{Value::Int(k), Value::Str(make_value(k, vsize))};
        // 8-byte big endian key so the lsm scans stay in order
        string key(8, '\0');
        uint64_t u = (uint64_t)k;
        for (int b = 0; b < 8; ++b) key[b] = char((u >> (8 * (7 - b))) & 0xFF);
        lsm.put(key, serialize_tuple(t, sch));
    }
    lsm.flush_memtable();
    auto t1 = Clock::now();

    mt19937_64 rng(123);
    int M = min(N, 50000);
    auto r0 = Clock::now();
    volatile int64_t sink = 0;
    for (int i = 0; i < M; ++i) {
        int64_t k = rng() % N;
        string key(8, '\0');
        uint64_t u = (uint64_t)k;
        for (int b = 0; b < 8; ++b) key[b] = char((u >> (8 * (7 - b))) & 0xFF);
        auto v = lsm.get(key);
        if (v) sink += v->size();
    }
    auto r1 = Clock::now();
    (void)sink;

    Result res;
    res.write_tput = N / secs(t0, t1);
    res.read_us = secs(r0, r1) / M * 1e6;
    res.bytes = dir_bytes(dir);
    return res;
}

int main(int argc, char** argv) {
    int vsize = argc > 1 ? atoi(argv[1]) : 32;
    printf("MiniDB Storage Benchmark: LSM vs B+ tree (value ~%d bytes)\n", vsize);
    printf("%-10s | %-22s | %-22s | %-18s\n", "rows", "write tput (ins/s)", "read latency (us)", "disk bytes");
    printf("%s\n", string(82, '-').c_str());

    for (int N : {10000, 50000, 100000, 200000}) {
        Result b = bench_btree(N, vsize);
        Result l = bench_lsm(N, vsize);
        printf("%-10d | B+:%9.0f LSM:%7.0f | B+:%7.2f LSM:%7.2f | B+:%8lld LSM:%8lld\n",
               N, b.write_tput, l.write_tput, b.read_us, l.read_us,
               (long long)b.bytes, (long long)l.bytes);
        printf("%-10s |  -> LSM %.2fx writes      |  -> B+ %.2fx faster reads |  -> LSM/B+ space %.2fx\n",
               "", l.write_tput / b.write_tput, l.read_us / b.read_us,
               (double)l.bytes / max<int64_t>(b.bytes, 1));
    }
    return 0;
}
