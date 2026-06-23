// test for mvcc: snapshot visibility rules. drives the MvccManager directly so
// we can set up exact txn orderings (a single process can't run two txns at the
// literally same time, so we control the ids/states by hand).
#include "mvcc.h"
#include <cstdio>
#include <cassert>
using namespace std;
using namespace minidb;

// little helper to make a version header
static VersionHeader ver(TxnId xmin, TxnId xmax) { return VersionHeader{xmin, xmax}; }

int main() {
    MvccManager m;

    // txn 1 inserts a row and commits.
    m.begin(1);
    VersionHeader rowA = ver(1, 0);   // created by txn 1, not deleted
    m.commit(1);

    // txn 2 starts AFTER txn1 committed -> should see rowA
    m.begin(2);
    Snapshot s2 = m.snapshot_for(2);
    assert(m.visible(rowA, s2));
    printf("[1] txn2 sees a row committed before it started: OK\n");

    // while txn2 is still open, txn3 inserts another row and commits.
    m.begin(3);
    VersionHeader rowB = ver(3, 0);
    m.commit(3);

    // txn2's snapshot was taken before txn3 existed -> must NOT see rowB.
    // this is the whole point of snapshot isolation.
    assert(!m.visible(rowB, s2));
    printf("[2] txn2 does NOT see a row committed after its snapshot: OK\n");

    // a brand new txn4 started now DOES see both rows.
    m.begin(4);
    Snapshot s4 = m.snapshot_for(4);
    assert(m.visible(rowA, s4));
    assert(m.visible(rowB, s4));
    printf("[3] later txn4 sees both committed rows: OK\n");

    // txn5 deletes rowA (stamps xmax=5) and commits.
    m.begin(5);
    VersionHeader rowA_deleted = ver(1, 5);
    m.commit(5);

    // txn4 took its snapshot before txn5 committed -> still sees rowA as live.
    assert(m.visible(rowA_deleted, s4));
    printf("[4] txn4's old snapshot still sees the row deleted by a later txn: OK\n");

    // a new reader sees it as gone.
    m.begin(6);
    Snapshot s6 = m.snapshot_for(6);
    assert(!m.visible(rowA_deleted, s6));
    printf("[5] new reader sees the deleted row as gone: OK\n");

    // aborted txn's writes are never visible.
    m.begin(7);
    VersionHeader rowC = ver(7, 0);
    m.abort(7);
    m.begin(8);
    Snapshot s8 = m.snapshot_for(8);
    assert(!m.visible(rowC, s8));
    printf("[6] aborted txn's row is invisible: OK\n");

    printf("MVCC TEST PASSED\n");
    return 0;
}
