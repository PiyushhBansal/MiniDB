// concurrency tests for the 2PL lock manager. checks S/S share, X blocks, deadlock detect
#include "lock_manager.h"
#include <thread>
#include <atomic>
#include <cstdio>
#include <cassert>
#include <chrono>
using namespace std;
using namespace minidb;

int main() {
    // 1. two shared locks should coexist
    {
        LockManager lm;
        ResourceId A{0, 0};
        lm.acquire(1, A, LockMode::SHARED);
        lm.acquire(2, A, LockMode::SHARED);  // shouldnt block
        printf("[1] two shared locks granted concurrently: OK\n");
        lm.release_all(1);
        lm.release_all(2);
    }

    // 2. X lock blocks until the holder lets go
    {
        LockManager lm;
        ResourceId A{0, 1};
        lm.acquire(1, A, LockMode::EXCLUSIVE);
        atomic<bool> got{false};
        thread t2([&] {
            lm.acquire(2, A, LockMode::EXCLUSIVE);  // waits for T1
            got = true;
            lm.release_all(2);
        });
        this_thread::sleep_for(chrono::milliseconds(100));
        assert(!got.load());  // still stuck waiting
        printf("[2] writer correctly blocked while X held...\n");
        lm.release_all(1);    // T2 can go now
        t2.join();
        assert(got.load());
        printf("[2] writer proceeded after release: OK\n");
    }

    // 3. deadlock detection
    {
        LockManager lm;
        ResourceId A{0, 2}, B{0, 3};
        atomic<int> victims{0};
        // T1 grabs A then wants B, T2 grabs B then wants A. classic deadlock
        lm.acquire(1, A, LockMode::EXCLUSIVE);
        lm.acquire(2, B, LockMode::EXCLUSIVE);
        thread t1([&] {
            try { lm.acquire(1, B, LockMode::EXCLUSIVE); }
            catch (const DeadlockAbort&) { victims++; lm.release_all(1); }
        });
        thread t2([&] {
            try { lm.acquire(2, A, LockMode::EXCLUSIVE); }
            catch (const DeadlockAbort&) { victims++; lm.release_all(2); }
        });
        t1.join();
        t2.join();
        printf("[3] deadlock victims aborted: %d (expected >=1)\n", victims.load());
        assert(victims.load() >= 1);
    }

    printf("LOCKING TEST PASSED\n");
    return 0;
}
