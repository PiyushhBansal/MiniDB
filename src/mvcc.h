// mvcc.h - multi version concurrency control (extension track B).
// idea: instead of locking rows for reads, every txn gets a "snapshot" of which
// other txns had already committed when it started. a row version is visible to
// a txn if its creator committed before the txn started and it hasn't been
// deleted (by someone committed-before-us). readers never block writers because
// they just check version stamps instead of taking locks.
#pragma once

#include "common.h"
#include "tuple.h"
#include <unordered_map>
#include <set>

using namespace std;

namespace minidb {

enum class TxnState { ACTIVE, COMMITTED, ABORTED };

// what a txn "sees": its own id + the set of txns that were still active (not
// yet committed) at the moment it started. anything in that set, or started
// later, is invisible to us.
struct Snapshot {
    TxnId xid = 0;          // the reading txn (0 for a throwaway read snapshot)
    set<TxnId> active;      // txns in-flight when we took the snapshot
    TxnId next = 0;         // first txn id not yet handed out when we started
};

class MvccManager {
public:
    void begin(TxnId t) {
        state_[t] = TxnState::ACTIVE;
        // remember everyone active right now - those writes won't be visible to us
        set<TxnId> act;
        for (auto& [id, st] : state_) if (st == TxnState::ACTIVE && id != t) act.insert(id);
        snap_[t] = Snapshot{t, act, t};
    }
    void commit(TxnId t) { state_[t] = TxnState::COMMITTED; }
    void abort(TxnId t)  { state_[t] = TxnState::ABORTED; }

    TxnState state(TxnId t) const {
        auto it = state_.find(t);
        return it == state_.end() ? TxnState::COMMITTED : it->second;  // unknown = old committed
    }

    // the snapshot a txn took at begin(). if we never saw it (autocommit write),
    // just make one on the spot.
    Snapshot snapshot_for(TxnId t) {
        auto it = snap_.find(t);
        if (it != snap_.end()) return it->second;
        Snapshot s; s.xid = t; s.next = t;
        for (auto& [id, st] : state_) if (st == TxnState::ACTIVE && id != t) s.active.insert(id);
        return s;
    }

    // a read-only SELECT outside any BEGIN. sees everything committed so far.
    Snapshot fresh_readonly_snapshot() {
        Snapshot s; s.xid = 0;
        for (auto& [id, st] : state_) if (st == TxnState::ACTIVE) s.active.insert(id);
        s.next = ~0ull;   // no upper bound - see all committed work up to now
        return s;
    }

    // the core rule: is this version visible to the given snapshot?
    bool visible(const VersionHeader& vh, const Snapshot& snap) const {
        // 1. is the version's creator visible to us?
        if (!committed_to_us(vh.xmin, snap)) return false;
        // 2. if it was deleted, and the deleter is visible to us, then it's gone
        if (vh.xmax != 0 && committed_to_us(vh.xmax, snap)) return false;
        return true;
    }

private:
    // "committed in our view" = our own txn, OR (it committed AND it didn't
    // start after us AND it wasn't still active when we took our snapshot).
    bool committed_to_us(TxnId t, const Snapshot& snap) const {
        if (t == 0) return false;
        if (t == snap.xid) return true;            // our own write
        if (t >= snap.next) return false;          // started after our snapshot
        if (snap.active.count(t)) return false;    // was in-flight when we started
        return state(t) == TxnState::COMMITTED;
    }

    unordered_map<TxnId, TxnState> state_;
    unordered_map<TxnId, Snapshot> snap_;
};

}  // namespace minidb
