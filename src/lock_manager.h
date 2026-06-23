// strict 2PL: hold all locks till commit. row-level (S for read, X for write).
// deadlocks: wait-for graph + DFS for a cycle, no timeouts.
#pragma once

#include "common.h"
#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <condition_variable>

using namespace std;

namespace minidb {

enum class LockMode { SHARED, EXCLUSIVE };

// victim throws this, engine catches it, rolls back and releases locks.
struct DeadlockAbort : DBError {
    explicit DeadlockAbort(TxnId t)
        : DBError("transaction " + to_string(t) + " aborted (deadlock victim)") {}
};

// resource = one row, identified by page id + slot.
struct ResourceId {
    PageId page_id;
    int16_t slot;
    bool operator==(const ResourceId& o) const {
        return page_id == o.page_id && slot == o.slot;
    }
};
struct ResourceIdHash {
    size_t operator()(const ResourceId& r) const {
        return hash<int64_t>()((int64_t(r.page_id) << 16) ^ r.slot);
    }
};

class LockManager {
public:
    // blocks until granted. throws DeadlockAbort if it would deadlock.
    void acquire(TxnId txn, const ResourceId& res, LockMode mode) {
        unique_lock<mutex> lk(mu_);
        auto& q = table_[res];

        // already hold something good enough? re-entry / upgrade
        for (auto& h : q.holders) {
            if (h.txn == txn) {
                if (mode == LockMode::SHARED || h.mode == LockMode::EXCLUSIVE) return;
                // upgrade S->X only if we're the only holder
                if (q.holders.size() == 1) { h.mode = LockMode::EXCLUSIVE; return; }
                break;  // other readers still here, wait them out
            }
        }

        while (!compatible(q, txn, mode)) {
            // txn waits-for every current holder
            for (auto& h : q.holders)
                if (h.txn != txn) wait_for_[txn].insert(h.txn);

            // requester is the victim - it's the one already blocked, cheapest to kill
            if (has_cycle(txn)) {
                wait_for_.erase(txn);
                throw DeadlockAbort(txn);
            }
            q.waiters++;
            cv_.wait(lk);
            q.waiters--;
        }
        wait_for_.erase(txn);
        q.holders.push_back({txn, mode});
        held_by_txn_[txn].insert(res);
    }

    // strict 2PL releases everything at once, on commit/abort.
    void release_all(TxnId txn) {
        unique_lock<mutex> lk(mu_);
        auto it = held_by_txn_.find(txn);
        if (it != held_by_txn_.end()) {
            for (auto& res : it->second) {
                auto qit = table_.find(res);
                if (qit == table_.end()) continue;
                auto& holders = qit->second.holders;
                holders.erase(remove_if(holders.begin(), holders.end(),
                              [&](const Holder& h) { return h.txn == txn; }), holders.end());
            }
            held_by_txn_.erase(it);
        }
        wait_for_.erase(txn);
        for (auto& [t, s] : wait_for_) s.erase(txn);
        cv_.notify_all();
    }

private:
    struct Holder { TxnId txn; LockMode mode; };
    struct LockQueue {
        vector<Holder> holders;
        int waiters = 0;
    };

    // S is only compatible with S
    bool compatible(const LockQueue& q, TxnId txn, LockMode mode) {
        for (auto& h : q.holders) {
            if (h.txn == txn) continue;
            if (mode == LockMode::EXCLUSIVE || h.mode == LockMode::EXCLUSIVE)
                return false;
        }
        return true;
    }

    // DFS the wait-for graph, back-edge means cycle
    bool has_cycle(TxnId start) {
        unordered_set<TxnId> visited, on_stack;
        return dfs(start, visited, on_stack);
    }
    bool dfs(TxnId u, unordered_set<TxnId>& visited, unordered_set<TxnId>& stack) {
        visited.insert(u);
        stack.insert(u);
        auto it = wait_for_.find(u);
        if (it != wait_for_.end()) {
            for (TxnId v : it->second) {
                if (stack.count(v)) return true;             // back-edge, cycle
                if (!visited.count(v) && dfs(v, visited, stack)) return true;
            }
        }
        stack.erase(u);
        return false;
    }

    mutex mu_;
    condition_variable cv_;
    unordered_map<ResourceId, LockQueue, ResourceIdHash> table_;
    unordered_map<TxnId, unordered_set<ResourceId, ResourceIdHash>> held_by_txn_;
    unordered_map<TxnId, unordered_set<TxnId>> wait_for_;  // adjacency list
};

}  // namespace minidb
