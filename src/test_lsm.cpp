// smoke test for the lsm tree engine
#include "lsm_tree.h"
#include <cstdio>
#include <cassert>
#include <random>
using namespace std;
using namespace minidb;

int main() {
    system("rm -rf /tmp/minidb_lsm && mkdir -p /tmp/minidb_lsm");
    // tiny limits so flush + compaction actually trigger without tons of data
    LSMTree lsm("/tmp/minidb_lsm", /*memtable_limit=*/100, /*compaction_threshold=*/3);

    const int N = 2000;
    for (int k = 0; k < N; ++k)
        lsm.put("key" + to_string(k), "val" + to_string(k));

    printf("after %d puts: %zu sstables, memtable=%zu, %d compactions\n",
           N, lsm.num_sstables(), lsm.memtable_size(), lsm.compactions_run());
    assert(lsm.num_sstables() >= 1);
    assert(lsm.compactions_run() >= 1);  // should have compacted by now

    // point lookups
    for (int k = 0; k < N; ++k) {
        auto v = lsm.get("key" + to_string(k));
        assert(v.has_value());
        assert(*v == "val" + to_string(k));
    }
    printf("point lookups OK\n");

    // overwrite + delete: newest wins, tombstone hides the value
    lsm.put("key10", "UPDATED");
    assert(*lsm.get("key10") == "UPDATED");
    lsm.remove("key20");
    assert(!lsm.get("key20").has_value());
    assert(!lsm.get("key_missing").has_value());
    printf("update + tombstone OK\n");

    // scan should give N-1 live keys (key20 deleted) in sorted order
    int count = 0; string last;
    lsm.scan([&](const string& k, const string&) {
        assert(last.empty() || last < k);
        last = k; count++; return true;
    });
    printf("scan yielded %d live keys (expected %d)\n", count, N - 1);
    assert(count == N - 1);

    printf("LSM TEST PASSED\n");
    return 0;
}
