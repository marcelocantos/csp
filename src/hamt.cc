#include <csp/internal/hamt.h>

#include <bit>


namespace csp::internal {

// --- Allocation ---

static hamt_inner* hamt_alloc_inner(int n_children) {
    void* p = operator new(sizeof(hamt_inner) + n_children * sizeof(uintptr_t));
    return new (p) hamt_inner{};
}

static void hamt_free_inner(hamt_inner* node) {
    node->~hamt_inner();
    operator delete(node);
}

// --- Release ---

void hamt_release(uintptr_t p) {
    if (!p) return;
    if (hamt_is_leaf(p)) {
        auto* leaf = hamt_to_leaf(p);
        if (leaf->rc.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete leaf;
        }
    } else {
        auto* inner = hamt_to_inner(p);
        if (inner->rc.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            int n = inner->child_count();
            for (int i = 0; i < n; i++) {
                hamt_release(inner->children()[i]);
            }
            hamt_free_inner(inner);
        }
    }
}

// --- Path-copy insert ---

// Create an inner node containing two leaves that diverge at the given shift.
static uintptr_t make_two_leaf_inner(
    hamt_leaf* existing, uint64_t new_key, std::any new_val, int shift
) {
    uint32_t idx1 = (existing->key >> shift) & 0x1f;
    uint32_t idx2 = (new_key >> shift) & 0x1f;

    if (idx1 == idx2) {
        // Same index at this level: recurse deeper.
        uintptr_t child = make_two_leaf_inner(existing, new_key, std::move(new_val), shift + 5);
        auto* inner = hamt_alloc_inner(1);
        inner->bitmap = 1u << idx1;
        inner->children()[0] = child;
        return hamt_from_inner(inner);
    }

    // Different indices: create inner with two children.
    auto* new_leaf = new hamt_leaf{{1}, new_key, std::move(new_val)};
    hamt_retain(hamt_from_leaf(existing));

    auto* inner = hamt_alloc_inner(2);
    inner->bitmap = (1u << idx1) | (1u << idx2);

    int pos1 = std::popcount(inner->bitmap & ((1u << idx1) - 1));
    int pos2 = std::popcount(inner->bitmap & ((1u << idx2) - 1));
    inner->children()[pos1] = hamt_from_leaf(existing);
    inner->children()[pos2] = hamt_from_leaf(new_leaf);

    return hamt_from_inner(inner);
}

static uintptr_t assoc_rec(uintptr_t node, uint64_t key, std::any value, int shift) {
    if (!node) {
        // Empty slot: create leaf.
        auto* leaf = new hamt_leaf{{1}, key, std::move(value)};
        return hamt_from_leaf(leaf);
    }

    if (hamt_is_leaf(node)) {
        auto* existing = hamt_to_leaf(node);
        if (existing->key == key) {
            // Replace: new leaf with updated value.
            auto* leaf = new hamt_leaf{{1}, key, std::move(value)};
            return hamt_from_leaf(leaf);
        }
        // Two different keys: push deeper until they diverge.
        return make_two_leaf_inner(existing, key, std::move(value), shift);
    }

    // Inner node: path-copy.
    auto* old = hamt_to_inner(node);
    uint32_t idx = (key >> shift) & 0x1f;
    uint32_t bit = 1u << idx;
    int pos = std::popcount(old->bitmap & (bit - 1));
    int n = old->child_count();

    if (old->bitmap & bit) {
        // Slot exists: recurse into child, replace in copy.
        uintptr_t new_child = assoc_rec(old->children()[pos], key, std::move(value), shift + 5);
        auto* copy = hamt_alloc_inner(n);
        copy->bitmap = old->bitmap;
        for (int i = 0; i < n; i++) {
            if (i == pos) {
                copy->children()[i] = new_child;
            } else {
                hamt_retain(old->children()[i]);
                copy->children()[i] = old->children()[i];
            }
        }
        return hamt_from_inner(copy);
    }

    // New slot: expand.
    auto* copy = hamt_alloc_inner(n + 1);
    copy->bitmap = old->bitmap | bit;
    auto* old_ch = old->children();
    auto* new_ch = copy->children();
    for (int i = 0; i < pos; i++) {
        hamt_retain(old_ch[i]);
        new_ch[i] = old_ch[i];
    }
    auto* leaf = new hamt_leaf{{1}, key, std::move(value)};
    new_ch[pos] = hamt_from_leaf(leaf);
    for (int i = pos; i < n; i++) {
        hamt_retain(old_ch[i]);
        new_ch[i + 1] = old_ch[i];
    }
    return hamt_from_inner(copy);
}

uintptr_t hamt_assoc(uintptr_t root, uint64_t key, std::any value) {
    return assoc_rec(root, key, std::move(value), 0);
}

} // namespace csp::internal
