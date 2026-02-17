#pragma once

// Persistent Hash Array Mapped Trie (HAMT) for dynamic-scoped variables.
// Keys are uint64_t (used as identity hash). Values are std::any.
// Nodes are immutable after creation; "mutation" is path-copy.
// Intrusive refcounting on all nodes (no shared_ptr).
//
// BLR-free read path: hamt_get uses only integer arithmetic,
// __builtin_popcount, and pointer chasing.

#include <any>
#include <atomic>
#include <cstdint>

namespace csp::internal {

// --- Node types ---

// Inner node: bitmap + variable-length children array (inline after struct).
struct hamt_inner {
    std::atomic<int> rc{1};
    uint32_t bitmap{0};

    uintptr_t* children() {
        return reinterpret_cast<uintptr_t*>(this + 1);
    }
    const uintptr_t* children() const {
        return reinterpret_cast<const uintptr_t*>(this + 1);
    }
    int child_count() const { return __builtin_popcount(bitmap); }
};

// Leaf node: key + type-erased value.
struct hamt_leaf {
    std::atomic<int> rc{1};
    uint64_t key;
    std::any value;
};

// --- Tagged pointer helpers ---
// Bit 0 = 1 means leaf, 0 means inner. 0 means empty.

inline bool hamt_is_leaf(uintptr_t p) { return p & 1; }

inline hamt_leaf* hamt_to_leaf(uintptr_t p) {
    return reinterpret_cast<hamt_leaf*>(p & ~uintptr_t(1));
}
inline hamt_inner* hamt_to_inner(uintptr_t p) {
    return reinterpret_cast<hamt_inner*>(p);
}
inline uintptr_t hamt_from_leaf(hamt_leaf* l) {
    return reinterpret_cast<uintptr_t>(l) | 1;
}
inline uintptr_t hamt_from_inner(hamt_inner* n) {
    return reinterpret_cast<uintptr_t>(n);
}

// --- Inline operations (hot path, BLR-free) ---

// Retain: both inner and leaf have atomic<int> rc as first member.
inline void hamt_retain(uintptr_t p) {
    reinterpret_cast<std::atomic<int>*>(p & ~uintptr_t(1))
        ->fetch_add(1, std::memory_order_relaxed);
}

// Lookup: returns pointer to value's any, or nullptr if not found.
inline const std::any* hamt_get(uintptr_t root, uint64_t key) {
    uintptr_t node = root;
    int shift = 0;
    while (node && !hamt_is_leaf(node)) {
        auto* inner = hamt_to_inner(node);
        uint32_t idx = (key >> shift) & 0x1f;
        uint32_t bit = 1u << idx;
        if (!(inner->bitmap & bit)) return nullptr;
        int pos = __builtin_popcount(inner->bitmap & (bit - 1));
        node = inner->children()[pos];
        shift += 5;
    }
    if (!node) return nullptr;
    auto* leaf = hamt_to_leaf(node);
    return (leaf->key == key) ? &leaf->value : nullptr;
}

// --- Cold-path declarations (implemented in hamt.cc) ---

// Path-copy insert/update. Returns new root (refcount 1).
// Caller must release the old root.
uintptr_t hamt_assoc(uintptr_t root, uint64_t key, std::any value);

// Recursive refcount release. Frees node when refcount reaches 0.
void hamt_release(uintptr_t p);

} // namespace csp::internal
