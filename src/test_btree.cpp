// smoke test for the b+ tree
#include "bplus_tree.h"
#include <cstdio>
#include <cassert>
#include <random>
#include <set>
using namespace std;
using namespace minidb;

int main() {
    remove("/tmp/minidb_btree_test.db");
    DiskManager dm("/tmp/minidb_btree_test.db");
    BufferPool bp(&dm, 64);

    printf("LEAF_MAX=%d INTERNAL_MAX=%d\n", LEAF_MAX, INTERNAL_MAX);

    PageId root = BPlusTree::create(&bp);
    BPlusTree tree(&bp, root);

    // shuffle the inserts so we actually hit splits all over the tree
    const int N = 5000;
    vector<int> keys;
    for (int i = 0; i < N; ++i) keys.push_back(i);
    mt19937 rng(42);
    shuffle(keys.begin(), keys.end(), rng);
    for (int k : keys) tree.insert(k, RecordId{k / 100, static_cast<int16_t>(k % 100)});

    // point lookups
    for (int k = 0; k < N; ++k) {
        auto r = tree.search(k);
        assert(r.has_value());
        assert(r->page_id == k / 100 && r->slot == k % 100);
    }
    assert(!tree.search(N + 1).has_value());
    printf("point lookups OK (%d keys)\n", N);

    // range scan [1000,1099], should be 100 keys in order
    int last = -1, cnt = 0;
    tree.range_scan(1000, 1099, [&](int64_t key, RecordId) {
        assert(key > last);
        last = key;
        cnt++;
        return true;
    });
    printf("range scan [1000,1099] yielded %d keys (expected 100)\n", cnt);
    assert(cnt == 100);

    // delete half, check theyre gone
    for (int k = 0; k < N; k += 2) assert(tree.erase(k));
    for (int k = 0; k < N; ++k) {
        auto r = tree.search(k);
        if (k % 2 == 0) assert(!r.has_value());
        else assert(r.has_value());
    }
    printf("delete OK\n");

    bp.flush_all();
    printf("BTREE TEST PASSED\n");
    return 0;
}
