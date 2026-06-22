// B+ tree on int64 keys -> RecordId. keys are PKs so fixed width is fine.
// each node = one page. we load a page into a node object, mutate, write back.
// slower than editing bytes in place but way easier to read/reason about.
#pragma once

#include "common.h"
#include "buffer_pool.h"
#include <cstring>
#include <vector>
#include <optional>
#include <algorithm>

using namespace std;

namespace minidb {

// on-page header. leaf: keys[i]->values[i], next = sibling leaf.
// internal: keys[i] splits children[i] (<key) and children[i+1] (>=key)
#pragma pack(push, 1)
struct BTreeNodeHeader {
    uint8_t is_leaf = 1;
    int16_t num_keys = 0;
    PageId next_leaf = INVALID_PAGE_ID;  // leaf linked list, for range scans
};
#pragma pack(pop)

// how many keys fit in a page. internal has one extra child ptr.
constexpr int LEAF_MAX = (PAGE_SIZE - sizeof(BTreeNodeHeader) - sizeof(PageId)) /
                         (sizeof(int64_t) + sizeof(RecordId));
constexpr int INTERNAL_MAX = (PAGE_SIZE - sizeof(BTreeNodeHeader) - sizeof(PageId)) /
                             (sizeof(int64_t) + sizeof(PageId));

struct BTreeNode {
    PageId page_id = INVALID_PAGE_ID;
    bool is_leaf = true;
    PageId next_leaf = INVALID_PAGE_ID;
    vector<int64_t> keys;
    vector<RecordId> values;   // leaf only
    vector<PageId> children;   // internal only, size == keys+1

    static BTreeNode load(BufferPool* bp, PageId pid) {
        Page* p = bp->fetch_page(pid);
        BTreeNode n;
        n.page_id = pid;
        auto* h = reinterpret_cast<BTreeNodeHeader*>(p->data);
        n.is_leaf = h->is_leaf != 0;
        n.next_leaf = h->next_leaf;
        const char* cur = p->data + sizeof(BTreeNodeHeader);
        for (int i = 0; i < h->num_keys; ++i) {
            int64_t key;
            memcpy(&key, cur, sizeof(key)); cur += sizeof(key);
            n.keys.push_back(key);
            if (n.is_leaf) {
                RecordId rid;
                memcpy(&rid, cur, sizeof(rid)); cur += sizeof(rid);
                n.values.push_back(rid);
            } else {
                PageId child;
                memcpy(&child, cur, sizeof(child)); cur += sizeof(child);
                n.children.push_back(child);
            }
        }
        if (!n.is_leaf) {
            PageId last;
            memcpy(&last, cur, sizeof(last));
            n.children.push_back(last);
        }
        bp->unpin_page(pid, false);
        return n;
    }

    void store(BufferPool* bp) const {
        Page* p = bp->fetch_page(page_id);
        auto* h = reinterpret_cast<BTreeNodeHeader*>(p->data);
        h->is_leaf = is_leaf ? 1 : 0;
        h->num_keys = static_cast<int16_t>(keys.size());
        h->next_leaf = next_leaf;
        char* cur = p->data + sizeof(BTreeNodeHeader);
        for (size_t i = 0; i < keys.size(); ++i) {
            memcpy(cur, &keys[i], sizeof(int64_t)); cur += sizeof(int64_t);
            if (is_leaf) {
                memcpy(cur, &values[i], sizeof(RecordId)); cur += sizeof(RecordId);
            } else {
                memcpy(cur, &children[i], sizeof(PageId)); cur += sizeof(PageId);
            }
        }
        if (!is_leaf) {
            PageId last = children.back();
            memcpy(cur, &last, sizeof(PageId));
        }
        bp->unpin_page(page_id, true);
    }
};

class BPlusTree {
public:
    BPlusTree(BufferPool* bp, PageId root) : bp_(bp), root_(root) {}

    static PageId create(BufferPool* bp) {
        PageId pid;
        Page* p = bp->new_page(&pid);
        auto* h = reinterpret_cast<BTreeNodeHeader*>(p->data);
        h->is_leaf = 1;
        h->num_keys = 0;
        h->next_leaf = INVALID_PAGE_ID;
        bp->unpin_page(pid, true);
        return pid;
    }

    PageId root() const { return root_; }

    optional<RecordId> search(int64_t key) {
        BTreeNode node = descend_to_leaf(key);
        auto it = lower_bound(node.keys.begin(), node.keys.end(), key);
        if (it != node.keys.end() && *it == key)
            return node.values[it - node.keys.begin()];
        return nullopt;
    }

    // calls fn(key, rid) for all keys in [lo, hi], in order
    template <typename Fn>
    void range_scan(int64_t lo, int64_t hi, Fn&& fn) {
        BTreeNode node = descend_to_leaf(lo);
        while (true) {
            for (size_t j = 0; j < node.keys.size(); ++j) {
                if (node.keys[j] < lo) continue;
                if (node.keys[j] > hi) return;
                if (!fn(node.keys[j], node.values[j])) return;
            }
            if (node.next_leaf == INVALID_PAGE_ID) return;
            node = BTreeNode::load(bp_, node.next_leaf);
        }
    }

    // splits propagate up. if the root splits, tree gets taller.
    void insert(int64_t key, RecordId rid) {
        int64_t split_key;
        PageId new_child;
        bool split = insert_rec(root_, key, rid, split_key, new_child);
        if (split) {
            // old root split, make a new root over both halves
            PageId new_root_pid;
            Page* p = bp_->new_page(&new_root_pid);
            bp_->unpin_page(new_root_pid, true);
            BTreeNode r;
            r.page_id = new_root_pid;
            r.is_leaf = false;
            r.keys = {split_key};
            r.children = {root_, new_child};
            r.store(bp_);
            root_ = new_root_pid;
        }
    }

    // lazy delete: just yank the key from its leaf. no merging on underflow.
    bool erase(int64_t key) {
        BTreeNode leaf = descend_to_leaf(key);
        auto it = lower_bound(leaf.keys.begin(), leaf.keys.end(), key);
        if (it == leaf.keys.end() || *it != key) return false;
        size_t idx = it - leaf.keys.begin();
        leaf.keys.erase(leaf.keys.begin() + idx);
        leaf.values.erase(leaf.values.begin() + idx);
        leaf.store(bp_);
        return true;
    }

private:
    // walk root -> leaf that should hold key
    BTreeNode descend_to_leaf(int64_t key) {
        BTreeNode node = BTreeNode::load(bp_, root_);
        while (!node.is_leaf) {
            size_t idx = 0;
            while (idx < node.keys.size() && key >= node.keys[idx]) ++idx;
            node = BTreeNode::load(bp_, node.children[idx]);
        }
        return node;
    }

    // returns true if this node split. then split_key/new_child = the new
    // right sibling for the parent to absorb.
    bool insert_rec(PageId pid, int64_t key, RecordId rid,
                    int64_t& split_key, PageId& new_child) {
        BTreeNode node = BTreeNode::load(bp_, pid);
        if (node.is_leaf) {
            auto it = lower_bound(node.keys.begin(), node.keys.end(), key);
            size_t idx = it - node.keys.begin();
            if (it != node.keys.end() && *it == key) {
                node.values[idx] = rid;  // key exists, just overwrite
                node.store(bp_);
                return false;
            }
            node.keys.insert(node.keys.begin() + idx, key);
            node.values.insert(node.values.begin() + idx, rid);
            if (static_cast<int>(node.keys.size()) <= LEAF_MAX) {
                node.store(bp_);
                return false;
            }
            return split_leaf(node, split_key, new_child);
        }
        // internal: recurse into the right child
        size_t n = 0;
        while (n < node.keys.size() && key >= node.keys[n]) ++n;
        int64_t child_split_key;
        PageId child_new;
        bool child_split = insert_rec(node.children[n], key, rid, child_split_key, child_new);
        if (!child_split) return false;
        // child split, absorb it here
        node.keys.insert(node.keys.begin() + n, child_split_key);
        node.children.insert(node.children.begin() + n + 1, child_new);
        if (static_cast<int>(node.keys.size()) <= INTERNAL_MAX) {
            node.store(bp_);
            return false;
        }
        return split_internal(node, split_key, new_child);
    }

    bool split_leaf(BTreeNode& node, int64_t& split_key, PageId& new_child) {
        size_t mid = node.keys.size() / 2;
        PageId right_pid;
        Page* p = bp_->new_page(&right_pid);
        bp_->unpin_page(right_pid, true);
        BTreeNode right;
        right.page_id = right_pid;
        right.is_leaf = true;
        right.keys.assign(node.keys.begin() + mid, node.keys.end());
        right.values.assign(node.values.begin() + mid, node.values.end());
        right.next_leaf = node.next_leaf;
        node.keys.resize(mid);
        node.values.resize(mid);
        node.next_leaf = right_pid;
        node.store(bp_);
        right.store(bp_);
        // leaf split copies the key up - it still lives in the right leaf
        split_key = right.keys.front();
        new_child = right_pid;
        return true;
    }

    bool split_internal(BTreeNode& node, int64_t& split_key, PageId& new_child) {
        size_t mid = node.keys.size() / 2;
        // internal split pushes the middle key up, so it's gone from here
        split_key = node.keys[mid];
        PageId right_pid;
        Page* p = bp_->new_page(&right_pid);
        bp_->unpin_page(right_pid, true);
        BTreeNode right;
        right.page_id = right_pid;
        right.is_leaf = false;
        right.keys.assign(node.keys.begin() + mid + 1, node.keys.end());
        right.children.assign(node.children.begin() + mid + 1, node.children.end());
        node.keys.resize(mid);
        node.children.resize(mid + 1);
        node.store(bp_);
        right.store(bp_);
        new_child = right_pid;
        return true;
    }

    BufferPool* bp_;
    PageId root_;
};

}  // namespace minidb
