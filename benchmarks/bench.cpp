// benchmark for the mvcc track: MVCC (lock-free reads) vs 2PL (locking reads)
// under read/write contention. we use the lock manager + a tiny mvcc manager
// directly so we can measure the blocking, without the full SQL path.
//
// the point of the track: under MVCC readers don't take locks, so a reader and
// a writer touching the same rows never block each other. under 2PL a reader's
// S-lock conflicts with the writer's X-lock, so somebody waits.
#include "../src/lock_manager.h"
#include "../src/mvcc.h"
#include <chrono>
#include <thread>
#include <atomic>
#include <cstdio>
using namespace std;
using namespace minidb;
using Clock = chrono::steady_clock;

static double ms(Clock::time_point a, Clock::time_point b) {
    return chrono::duration<double, milli>(b - a).count();
}

// 2PL: a reader takes an S-lock on each row it reads, holds till "commit".
// while a writer holds the X-lock, the reader blocks. we measure total reader
// time while a writer churns the same rows.
double bench_2pl(int rows, int reads_per_row) {
    LockManager lm;
    atomic<bool> stop{false};
    // writer thread: keep grabbing X-locks on the rows and releasing
    thread writer([&]{
        TxnId w = 1000000;
        while (!stop.load()) {
            for (int r = 0; r < rows; ++r) {
                lm.acquire(w, ResourceId{0, (int16_t)r}, LockMode::EXCLUSIVE);
            }
            lm.release_all(w);
        }
    });
    auto t0 = Clock::now();
    // reader: S-lock each row, read it, repeat. blocks whenever writer holds X.
    for (int pass = 0; pass < reads_per_row; ++pass) {
        TxnId rd = 2000000 + pass;
        for (int r = 0; r < rows; ++r)
            lm.acquire(rd, ResourceId{0, (int16_t)r}, LockMode::SHARED);
        lm.release_all(rd);
    }
    auto t1 = Clock::now();
    stop = true;
    writer.join();
    return ms(t0, t1);
}

// MVCC: reader takes NO locks, just checks visibility. writer still X-locks for
// writes but that never blocks the reader.
double bench_mvcc(int rows, int reads_per_row) {
    LockManager lm;
    MvccManager mv;
    atomic<bool> stop{false};
    thread writer([&]{
        TxnId w = 1000000;
        while (!stop.load()) {
            // writer locks rows it writes (write-write still needs protection)
            for (int r = 0; r < rows; ++r)
                lm.acquire(w, ResourceId{0, (int16_t)r}, LockMode::EXCLUSIVE);
            lm.release_all(w);
        }
    });
    auto t0 = Clock::now();
    for (int pass = 0; pass < reads_per_row; ++pass) {
        mv.begin(3000000 + pass);
        Snapshot s = mv.snapshot_for(3000000 + pass);
        for (int r = 0; r < rows; ++r) {
            // no lock! just a visibility check on the version header.
            VersionHeader vh{1, 0};
            volatile bool vis = mv.visible(vh, s);
            (void)vis;
        }
        mv.commit(3000000 + pass);
    }
    auto t1 = Clock::now();
    stop = true;
    writer.join();
    return ms(t0, t1);
}

int main() {
    printf("MVCC vs 2PL under read/write contention\n");
    printf("(reader does N passes over R rows while a writer holds X-locks on them)\n\n");
    printf("%-8s | %-8s | %-14s | %-14s | %s\n", "rows", "passes", "2PL reads (ms)", "MVCC reads(ms)", "speedup");
    printf("%s\n", string(70, '-').c_str());
    for (int rows : {50, 100, 200}) {
        int passes = 200;
        double t2pl = bench_2pl(rows, passes);
        double tmv = bench_mvcc(rows, passes);
        printf("%-8d | %-8d | %-14.2f | %-14.2f | %.1fx\n",
               rows, passes, t2pl, tmv, t2pl / tmv);
    }
    printf("\nMVCC readers never block (no S-locks), so reads stay fast under a busy writer.\n");
    return 0;
}
