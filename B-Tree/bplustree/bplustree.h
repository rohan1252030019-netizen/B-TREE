#pragma once
#ifndef BPLUSTREE_H
#define BPLUSTREE_H

/*
 * BPlusTree<K, V, t>
 *
 * Generic B+ Tree template.
 *   K  = key type (must support < and == operators)
 *   V  = value type stored in leaf payloads
 *   t  = minimum degree (default 4, so max 2t-1 = 7 keys per node)
 *
 * All records reside in leaf nodes; internal nodes hold separator keys
 * and child pointers only.  Leaves are doubly-linked for O(n) range scans.
 *
 * Thread safety: Reader-Writer lock (SRWLOCK) guards all operations.
 *   Multiple concurrent readers are allowed; writes are exclusive.
 *
 * Duplicate keys: resolved by appending a tie-breaking path string so that
 * every (key, tiebreak) pair is unique in the tree.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cassert>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Forward declarations of animation callback types
// ---------------------------------------------------------------------------
struct SplitEvent  { void* nodePtr; int promotedKeyIdx; };
struct MergeEvent  { void* leftPtr; void* rightPtr; };
struct SearchEvent { std::vector<void*> traversedNodes; };

// ---------------------------------------------------------------------------
// Node types
// ---------------------------------------------------------------------------
template<typename K, typename V, int t = 4>
struct BPTNode {
    static constexpr int MAX_KEYS     = 2 * t - 1;
    static constexpr int MIN_KEYS     = t - 1;
    static constexpr int MAX_CHILDREN = 2 * t;

    bool                     isLeaf;
    int                      numKeys;
    K                        keys[2 * t];          // separator keys
    std::wstring             tiebreaks[2 * t];     // tie-break paths (leaves only)
    V                        values[2 * t];         // payloads (leaves only)
    BPTNode<K, V, t>*        children[2 * t + 1];  // child pointers (internal only)
    BPTNode<K, V, t>*        next;                  // next leaf (doubly-linked)
    BPTNode<K, V, t>*        prev;                  // prev leaf

    explicit BPTNode(bool leaf)
        : isLeaf(leaf), numKeys(0), next(nullptr), prev(nullptr)
    {
        for (int i = 0; i <= MAX_KEYS + 1; ++i) children[i] = nullptr;
    }
};

// ---------------------------------------------------------------------------
// Main BPlusTree class
// ---------------------------------------------------------------------------
template<typename K, typename V, int t = 4>
class BPlusTree {
public:
    using Node        = BPTNode<K, V, t>;
    using KeyVal      = std::pair<K, V>;

    // Callbacks fired after structural changes (used by visualizer)
    std::function<void(SplitEvent)>  onSplit;
    std::function<void(MergeEvent)>  onMerge;
    std::function<void(SearchEvent)> onSearch;

    // -----------------------------------------------------------------------
    BPlusTree() : root_(nullptr), size_(0) {
        InitializeSRWLock(&srwLock_);
        root_ = new Node(true);
    }

    ~BPlusTree() {
        clearNode(root_);
    }

    // -----------------------------------------------------------------------
    // Disallow copy; allow move
    BPlusTree(const BPlusTree&)            = delete;
    BPlusTree& operator=(const BPlusTree&) = delete;

    BPlusTree(BPlusTree&& o) noexcept
        : root_(o.root_), size_(o.size_)
    {
        InitializeSRWLock(&srwLock_);
        o.root_ = nullptr;
        o.size_ = 0;
    }

    // -----------------------------------------------------------------------
    // Returns number of records in the tree
    size_t size() const {
        AcquireSRWLockShared(const_cast<SRWLOCK*>(&srwLock_));
        size_t s = size_;
        ReleaseSRWLockShared(const_cast<SRWLOCK*>(&srwLock_));
        return s;
    }

    // Returns height of the tree (1 = root is a leaf)
    int height() const {
        AcquireSRWLockShared(const_cast<SRWLOCK*>(&srwLock_));
        int h = computeHeight(root_);
        ReleaseSRWLockShared(const_cast<SRWLOCK*>(&srwLock_));
        return h;
    }

    // -----------------------------------------------------------------------
    // Insert a (key, tiebreak, value) triple.
    // tiebreak is the full path string – guarantees uniqueness of duplicates.
    void insert(const K& key, const std::wstring& tiebreak, const V& value) {
        AcquireSRWLockExclusive(&srwLock_);

        if (root_->numKeys == Node::MAX_KEYS) {
            // Root is full – grow tree height
            Node* oldRoot = root_;
            Node* newRoot = new Node(false);
            newRoot->children[0] = oldRoot;
            splitChild(newRoot, 0, oldRoot);
            root_ = newRoot;
        }
        insertNonFull(root_, key, tiebreak, value);
        ++size_;

        ReleaseSRWLockExclusive(&srwLock_);
    }

    // -----------------------------------------------------------------------
    // Remove entry identified by (key, tiebreak).
    // Returns true if found and removed.
    bool remove(const K& key, const std::wstring& tiebreak) {
        AcquireSRWLockExclusive(&srwLock_);
        bool found = removeFromNode(root_, key, tiebreak);
        if (found) {
            --size_;
            // Shrink tree if root became empty internal node
            if (!root_->isLeaf && root_->numKeys == 0) {
                Node* oldRoot = root_;
                root_ = root_->children[0];
                delete oldRoot;
            }
        }
        ReleaseSRWLockExclusive(&srwLock_);
        return found;
    }

    // -----------------------------------------------------------------------
    // Exact search by (key, tiebreak). Returns optional<V>.
    std::optional<V> search(const K& key, const std::wstring& tiebreak) const {
        AcquireSRWLockShared(const_cast<SRWLOCK*>(&srwLock_));
        std::vector<Node*> traversed;
        std::optional<V> result = searchNode(root_, key, tiebreak, traversed);
        if (onSearch) {
            SearchEvent ev;
            for (auto* n : traversed) ev.traversedNodes.push_back(static_cast<void*>(n));
            // Fire callback outside lock to avoid re-entrancy issues
        }
        ReleaseSRWLockShared(const_cast<SRWLOCK*>(&srwLock_));
        if (onSearch) {
            SearchEvent ev;
            for (auto* n : traversed) ev.traversedNodes.push_back(static_cast<void*>(n));
            onSearch(ev);
        }
        return result;
    }

    // -----------------------------------------------------------------------
    // Range query: collect all values with key in [low, high].
    // Optionally provide a comparator for tiebreak disambiguation.
    std::vector<V> rangeQuery(const K& low, const K& high) const {
        AcquireSRWLockShared(const_cast<SRWLOCK*>(&srwLock_));
        std::vector<V> results;
        rangeQueryImpl(root_, low, high, results);
        ReleaseSRWLockShared(const_cast<SRWLOCK*>(&srwLock_));
        return results;
    }

    // -----------------------------------------------------------------------
    // Prefix search: for string keys only – returns values whose key starts
    // with 'prefix'. Uses range scan on leaf linked-list.
    std::vector<V> prefixSearch(const K& prefix) const {
        AcquireSRWLockShared(const_cast<SRWLOCK*>(&srwLock_));
        std::vector<V> results;
        prefixSearchImpl(root_, prefix, results);
        ReleaseSRWLockShared(const_cast<SRWLOCK*>(&srwLock_));
        return results;
    }

    // -----------------------------------------------------------------------
    // In-order traversal of the leaf linked-list (ascending).
    std::vector<V> traverseAscending() const {
        AcquireSRWLockShared(const_cast<SRWLOCK*>(&srwLock_));
        std::vector<V> result;
        Node* leaf = leftmostLeaf(root_);
        while (leaf) {
            for (int i = 0; i < leaf->numKeys; ++i)
                result.push_back(leaf->values[i]);
            leaf = leaf->next;
        }
        ReleaseSRWLockShared(const_cast<SRWLOCK*>(&srwLock_));
        return result;
    }

    // In-order traversal descending (right-to-left leaf traversal)
    std::vector<V> traverseDescending() const {
        AcquireSRWLockShared(const_cast<SRWLOCK*>(&srwLock_));
        std::vector<V> result;
        Node* leaf = rightmostLeaf(root_);
        while (leaf) {
            for (int i = leaf->numKeys - 1; i >= 0; --i)
                result.push_back(leaf->values[i]);
            leaf = leaf->prev;
        }
        ReleaseSRWLockShared(const_cast<SRWLOCK*>(&srwLock_));
        return result;
    }

    // -----------------------------------------------------------------------
    // Snapshot of tree structure for the visualizer (takes read lock)
    struct NodeSnapshot {
        bool              isLeaf;
        int               numKeys;
        std::vector<K>    keys;
        bool              hasNext; // leaf linkage
        void*             nodePtr;
        std::vector<NodeSnapshot> children;
    };

    NodeSnapshot snapshot() const {
        AcquireSRWLockShared(const_cast<SRWLOCK*>(&srwLock_));
        NodeSnapshot snap = snapshotNode(root_);
        ReleaseSRWLockShared(const_cast<SRWLOCK*>(&srwLock_));
        return snap;
    }

    // Clear all entries
    void clear() {
        AcquireSRWLockExclusive(&srwLock_);
        clearNode(root_);
        root_  = new Node(true);
        size_  = 0;
        ReleaseSRWLockExclusive(&srwLock_);
    }

    // -----------------------------------------------------------------------
private:
    Node*   root_;
    size_t  size_;
    SRWLOCK srwLock_;

    // ------ Helpers ---------------------------------------------------------

    static int computeHeight(const Node* n) {
        if (!n) return 0;
        if (n->isLeaf) return 1;
        return 1 + computeHeight(n->children[0]);
    }

    static Node* leftmostLeaf(Node* n) {
        while (n && !n->isLeaf) n = n->children[0];
        return n;
    }

    static Node* rightmostLeaf(Node* n) {
        while (n && !n->isLeaf) n = n->children[n->numKeys];
        return n;
    }

    static void clearNode(Node* n) {
        if (!n) return;
        if (!n->isLeaf)
            for (int i = 0; i <= n->numKeys; ++i)
                clearNode(n->children[i]);
        delete n;
    }

    // Binary search: returns index of first key >= k
    static int lowerBound(const Node* n, const K& k, const std::wstring& tb) {
        int lo = 0, hi = n->numKeys;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (n->keys[mid] < k || (!(k < n->keys[mid]) && n->tiebreaks[mid] < tb))
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    }

    // For range queries we only compare by key (ignore tiebreak)
    static int lowerBoundKey(const Node* n, const K& k) {
        int lo = 0, hi = n->numKeys;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (n->keys[mid] < k) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    // ------ Insert ----------------------------------------------------------

    void insertNonFull(Node* node, const K& key, const std::wstring& tb, const V& value) {
        if (node->isLeaf) {
            // Find insertion position using composite key
            int i = lowerBound(node, key, tb);
            // Shift right
            for (int j = node->numKeys; j > i; --j) {
                node->keys[j]      = node->keys[j - 1];
                node->tiebreaks[j] = node->tiebreaks[j - 1];
                node->values[j]    = node->values[j - 1];
            }
            node->keys[i]      = key;
            node->tiebreaks[i] = tb;
            node->values[i]    = value;
            node->numKeys++;
        } else {
            // Find child to descend into
            int i = node->numKeys;
            while (i > 0 && (key < node->keys[i - 1] ||
                   (!(node->keys[i-1] < key) && tb < node->tiebreaks[i - 1])))
                --i;

            Node* child = node->children[i];
            if (child->numKeys == Node::MAX_KEYS) {
                splitChild(node, i, child);
                // After split, decide which new child to descend into
                if (key > node->keys[i] ||
                    (!(key < node->keys[i]) && !(node->keys[i] < key) && tb >= node->tiebreaks[i]))
                    ++i;
            }
            insertNonFull(node->children[i], key, tb, value);
        }
    }

    void splitChild(Node* parent, int childIdx, Node* child) {
        Node* sibling = new Node(child->isLeaf);
        int   mid     = t - 1; // median index in child

        // Promoted key (for internal node, this key moves up and is NOT in sibling)
        K              promotedKey = child->keys[mid];
        std::wstring   promotedTb  = child->tiebreaks[mid];

        if (child->isLeaf) {
            // Leaf split: promoted key stays in sibling too (B+ Tree property)
            sibling->numKeys = child->numKeys - mid;   // t keys
            for (int j = 0; j < sibling->numKeys; ++j) {
                sibling->keys[j]      = child->keys[mid + j];
                sibling->tiebreaks[j] = child->tiebreaks[mid + j];
                sibling->values[j]    = child->values[mid + j];
            }
            child->numKeys = mid; // t-1 keys

            // Fix leaf linked list
            sibling->next = child->next;
            sibling->prev = child;
            if (child->next) child->next->prev = sibling;
            child->next   = sibling;
        } else {
            // Internal split: median key promoted, not duplicated
            sibling->numKeys = child->numKeys - mid - 1;
            for (int j = 0; j < sibling->numKeys; ++j) {
                sibling->keys[j]      = child->keys[mid + 1 + j];
                sibling->tiebreaks[j] = child->tiebreaks[mid + 1 + j];
            }
            for (int j = 0; j <= sibling->numKeys; ++j)
                sibling->children[j] = child->children[mid + 1 + j];
            child->numKeys = mid;
        }

        // Insert promoted key into parent
        for (int j = parent->numKeys; j > childIdx; --j) {
            parent->keys[j]       = parent->keys[j - 1];
            parent->tiebreaks[j]  = parent->tiebreaks[j - 1];
            parent->children[j+1] = parent->children[j];
        }
        parent->keys[childIdx]       = promotedKey;
        parent->tiebreaks[childIdx]  = promotedTb;
        parent->children[childIdx+1] = sibling;
        parent->numKeys++;

        // Fire split animation callback
        if (onSplit) {
            SplitEvent ev;
            ev.nodePtr        = static_cast<void*>(child);
            ev.promotedKeyIdx = childIdx;
            onSplit(ev);
        }
    }

    // ------ Delete ----------------------------------------------------------

    bool removeFromNode(Node* node, const K& key, const std::wstring& tb) {
        if (node->isLeaf) {
            int i = lowerBound(node, key, tb);
            if (i < node->numKeys && !(key < node->keys[i]) && !(node->keys[i] < key)
                && node->tiebreaks[i] == tb)
            {
                // Found – shift left
                for (int j = i; j < node->numKeys - 1; ++j) {
                    node->keys[j]      = node->keys[j + 1];
                    node->tiebreaks[j] = node->tiebreaks[j + 1];
                    node->values[j]    = node->values[j + 1];
                }
                node->numKeys--;
                return true;
            }
            return false;
        }

        // Internal node
        int i = lowerBoundKey(node, key);
        // Adjust for tiebreak at exact key match boundary
        if (i < node->numKeys && !(key < node->keys[i]) && !(node->keys[i] < key)) {
            // Could be in child i or child i+1; check tiebreak
            if (tb >= node->tiebreaks[i]) ++i;
        }

        Node* child = node->children[i];
        if (child->numKeys <= Node::MIN_KEYS)
            rebalanceChild(node, i);

        // After rebalance, root may have changed pointers; find child again
        // (rebalanceChild may have merged, shifting children)
        // We re-search conservatively:
        return removeFromNode(node->children[findChildIndex(node, key, tb)], key, tb);
    }

    // Find appropriate child index (re-computed after rebalance)
    int findChildIndex(const Node* node, const K& key, const std::wstring& tb) const {
        int i = lowerBoundKey(node, key);
        if (i < node->numKeys && !(key < node->keys[i]) && !(node->keys[i] < key)) {
            if (tb >= node->tiebreaks[i]) ++i;
        }
        return i;
    }

    // Ensure child[i] has at least t keys before descending into it
    void rebalanceChild(Node* parent, int i) {
        // Try borrow from left sibling
        if (i > 0 && parent->children[i - 1]->numKeys > Node::MIN_KEYS) {
            borrowFromLeft(parent, i);
            return;
        }
        // Try borrow from right sibling
        if (i < parent->numKeys && parent->children[i + 1]->numKeys > Node::MIN_KEYS) {
            borrowFromRight(parent, i);
            return;
        }
        // Merge
        if (i > 0) {
            mergeChildren(parent, i - 1);   // merge child[i-1] and child[i]
        } else {
            mergeChildren(parent, i);        // merge child[i] and child[i+1]
        }
    }

    void borrowFromLeft(Node* parent, int i) {
        Node* child  = parent->children[i];
        Node* leftSib = parent->children[i - 1];

        // Shift child right by 1
        for (int j = child->numKeys; j > 0; --j) {
            child->keys[j]      = child->keys[j - 1];
            child->tiebreaks[j] = child->tiebreaks[j - 1];
            if (child->isLeaf) child->values[j] = child->values[j - 1];
        }
        if (!child->isLeaf) {
            for (int j = child->numKeys + 1; j > 0; --j)
                child->children[j] = child->children[j - 1];
        }

        if (child->isLeaf) {
            // Take last key of left sibling directly
            child->keys[0]      = leftSib->keys[leftSib->numKeys - 1];
            child->tiebreaks[0] = leftSib->tiebreaks[leftSib->numKeys - 1];
            child->values[0]    = leftSib->values[leftSib->numKeys - 1];
            // Update separator in parent
            parent->keys[i - 1]      = child->keys[0];
            parent->tiebreaks[i - 1] = child->tiebreaks[0];
        } else {
            // Pull separator down
            child->keys[0]      = parent->keys[i - 1];
            child->tiebreaks[0] = parent->tiebreaks[i - 1];
            child->children[0]  = leftSib->children[leftSib->numKeys];
            // Promote last key of left sibling
            parent->keys[i - 1]      = leftSib->keys[leftSib->numKeys - 1];
            parent->tiebreaks[i - 1] = leftSib->tiebreaks[leftSib->numKeys - 1];
        }
        leftSib->numKeys--;
        child->numKeys++;
    }

    void borrowFromRight(Node* parent, int i) {
        Node* child    = parent->children[i];
        Node* rightSib = parent->children[i + 1];

        if (child->isLeaf) {
            child->keys[child->numKeys]      = rightSib->keys[0];
            child->tiebreaks[child->numKeys] = rightSib->tiebreaks[0];
            child->values[child->numKeys]    = rightSib->values[0];
            // Shift right sibling left
            for (int j = 0; j < rightSib->numKeys - 1; ++j) {
                rightSib->keys[j]      = rightSib->keys[j + 1];
                rightSib->tiebreaks[j] = rightSib->tiebreaks[j + 1];
                rightSib->values[j]    = rightSib->values[j + 1];
            }
            // Update separator
            parent->keys[i]      = rightSib->keys[0];
            parent->tiebreaks[i] = rightSib->tiebreaks[0];
        } else {
            child->keys[child->numKeys]          = parent->keys[i];
            child->tiebreaks[child->numKeys]     = parent->tiebreaks[i];
            child->children[child->numKeys + 1]  = rightSib->children[0];
            parent->keys[i]      = rightSib->keys[0];
            parent->tiebreaks[i] = rightSib->tiebreaks[0];
            // Shift right sibling left
            for (int j = 0; j < rightSib->numKeys - 1; ++j) {
                rightSib->keys[j]      = rightSib->keys[j + 1];
                rightSib->tiebreaks[j] = rightSib->tiebreaks[j + 1];
            }
            for (int j = 0; j < rightSib->numKeys; ++j)
                rightSib->children[j] = rightSib->children[j + 1];
        }
        child->numKeys++;
        rightSib->numKeys--;
    }

    // Merge children[i] and children[i+1] (with separator from parent at keys[i])
    void mergeChildren(Node* parent, int i) {
        Node* left  = parent->children[i];
        Node* right = parent->children[i + 1];

        if (!left->isLeaf) {
            // Pull separator key down into left
            left->keys[left->numKeys]      = parent->keys[i];
            left->tiebreaks[left->numKeys] = parent->tiebreaks[i];
            left->numKeys++;
        }

        // Copy right into left
        for (int j = 0; j < right->numKeys; ++j) {
            left->keys[left->numKeys]      = right->keys[j];
            left->tiebreaks[left->numKeys] = right->tiebreaks[j];
            if (left->isLeaf) left->values[left->numKeys] = right->values[j];
            left->numKeys++;
        }
        if (!left->isLeaf) {
            for (int j = 0; j <= right->numKeys; ++j)
                left->children[left->numKeys - right->numKeys + j] = right->children[j];
        }

        // Fix leaf linkage
        if (left->isLeaf) {
            left->next = right->next;
            if (right->next) right->next->prev = left;
        }

        // Remove separator from parent
        for (int j = i; j < parent->numKeys - 1; ++j) {
            parent->keys[j]       = parent->keys[j + 1];
            parent->tiebreaks[j]  = parent->tiebreaks[j + 1];
            parent->children[j+1] = parent->children[j + 2];
        }
        parent->numKeys--;

        if (onMerge) {
            MergeEvent ev;
            ev.leftPtr  = static_cast<void*>(left);
            ev.rightPtr = static_cast<void*>(right);
            onMerge(ev);
        }

        delete right;
    }

    // ------ Search ----------------------------------------------------------

    std::optional<V> searchNode(Node* node, const K& key, const std::wstring& tb,
                                std::vector<Node*>& traversed) const
    {
        traversed.push_back(node);
        if (node->isLeaf) {
            int i = lowerBound(node, key, tb);
            if (i < node->numKeys && !(key < node->keys[i]) && !(node->keys[i] < key)
                && node->tiebreaks[i] == tb)
                return node->values[i];
            return std::nullopt;
        }
        int i = lowerBoundKey(node, key);
        if (i < node->numKeys && !(key < node->keys[i]) && !(node->keys[i] < key)) {
            if (tb >= node->tiebreaks[i]) ++i;
        }
        return searchNode(node->children[i], key, tb, traversed);
    }

    // ------ Range query -----------------------------------------------------

    void rangeQueryImpl(Node* node, const K& low, const K& high,
                        std::vector<V>& results) const
    {
        if (node->isLeaf) {
            int i = lowerBoundKey(node, low);
            while (i < node->numKeys && !(high < node->keys[i])) {
                results.push_back(node->values[i]);
                ++i;
            }
            if (i == node->numKeys && node->next)
                rangeQueryLeaf(node->next, high, results);
        } else {
            int i = lowerBoundKey(node, low);
            rangeQueryImpl(node->children[i], low, high, results);
        }
    }

    static void rangeQueryLeaf(Node* leaf, const K& high, std::vector<V>& results) {
        while (leaf) {
            for (int i = 0; i < leaf->numKeys; ++i) {
                if (high < leaf->keys[i]) return;
                results.push_back(leaf->values[i]);
            }
            leaf = leaf->next;
        }
    }

    // ------ Prefix search (string keys) -------------------------------------

    void prefixSearchImpl(Node* node, const K& prefix, std::vector<V>& results) const {
        // Navigate to first key >= prefix
        if (node->isLeaf) {
            // Walk from here rightward while key starts with prefix
            Node* leaf = node;
            while (leaf) {
                bool anyMatch = false;
                for (int i = 0; i < leaf->numKeys; ++i) {
                    const K& k = leaf->keys[i];
                    // Check if k starts with prefix (string comparison)
                    if (k.size() >= prefix.size() &&
                        k.substr(0, prefix.size()) == prefix)
                    {
                        results.push_back(leaf->values[i]);
                        anyMatch = true;
                    } else if (anyMatch) {
                        // Past the prefix zone
                        return;
                    } else if (k > prefix) {
                        return; // No more matches possible
                    }
                }
                leaf = leaf->next;
            }
        } else {
            int i = lowerBoundKey(node, prefix);
            if (i > 0) --i; // go one left to not miss keys with same prefix
            prefixSearchImpl(node->children[i], prefix, results);
        }
    }

    // ------ Snapshot --------------------------------------------------------

    NodeSnapshot snapshotNode(const Node* n) const {
        NodeSnapshot snap;
        snap.isLeaf  = n->isLeaf;
        snap.numKeys = n->numKeys;
        snap.nodePtr = const_cast<void*>(static_cast<const void*>(n));
        snap.hasNext = (n->isLeaf && n->next != nullptr);
        for (int i = 0; i < n->numKeys; ++i)
            snap.keys.push_back(n->keys[i]);
        if (!n->isLeaf) {
            for (int i = 0; i <= n->numKeys; ++i)
                snap.children.push_back(snapshotNode(n->children[i]));
        }
        return snap;
    }
};

#endif // BPLUSTREE_H
