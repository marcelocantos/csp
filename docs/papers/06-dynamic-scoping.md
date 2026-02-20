# Dynamic Scoping for Imps

## Abstract

Thread-local storage is the standard mechanism for per-thread state
in C++. But in an M:N imp-based concurrency system, where thousands of
imps are multiplexed across a handful of OS threads,
thread-local storage refers to the wrong entity — the OS thread, not
the imp. We describe a dynamic scoping system for
imps, built on a persistent hash array mapped trie (HAMT)
with intrusive reference counting. Variables are inherited from parent
to child on spawn, isolated by copy-on-write, and sendable over
channels as first-class values. The read path compiles to pure inline
arithmetic with zero indirect calls.

## 1. The problem with thread-local storage

C++ `thread_local` variables are indexed by OS thread. In a
single-threaded-per-task model, this works: each task has its own
thread, and `thread_local` is effectively per-task.

In an M:N scheduler, the mapping breaks. A imp running on OS
thread A may be suspended and resumed on OS thread B. If it wrote to
a `thread_local` variable before suspending, the value is on thread
A's TLS. After resuming on thread B, the read hits thread B's TLS —
a different slot with a different value. The imp's "per-thread"
state has silently changed identity.

Go solves this by not offering thread-local storage at all.
`runtime.getg()` returns the goroutine pointer, and goroutine-local
data is stored on the goroutine struct. But Go is a language runtime
with compiler support; a C++ library cannot add fields to a function's
local scope.

The question for a C++ imp-based concurrency library is: what mechanism can
provide per-imp state that follows the imp across OS
threads, is inherited by child imps, and doesn't require
compiler support?

## 2. Dynamic scoping

The answer comes from Lisp. Dynamic scoping — as opposed to lexical
scoping — binds variables in a way that is visible to all functions
called within the binding's scope, regardless of where those
functions are defined. Common Lisp's `special` variables and Clojure's
`binding` macro are the canonical examples.

For imps, dynamic scoping has three desirable properties:

1. **Inheritance.** A child imp inherits its parent's
   bindings. A trace ID set at the top of a request handler is
   visible in every function called within that handler, including
   functions in spawned child imps.

2. **Isolation.** A child that modifies a binding does not affect the
   parent. The parent's bindings remain unchanged — the child gets
   its own copy of the modified binding.

3. **Scope-based cleanup.** Bindings can be saved and restored using
   RAII, matching C++'s resource management idiom.

The challenge is implementing this efficiently. A naive approach —
copying the entire binding map on every spawn or write — would be
prohibitively expensive for imps that are created and
destroyed at high rates.

## 3. The persistent HAMT

The data structure behind CSP's dynamic scoping is a persistent
hash array mapped trie (HAMT) with intrusive reference counting.

### 3.1 Structure

A HAMT is a tree indexed by the bits of the key. At each level, 5
bits of the key select one of 32 possible children. A 32-bit bitmap
records which children are present, and the children are stored in a
compact array (only present children are allocated). For a 64-bit
key, the tree has at most 13 levels.

```
Root (inner node)
  bitmap: 0b...00100010
  children: [child_at_bit_1, child_at_bit_5]
```

Each node is either an inner node (bitmap + children array) or a
leaf (key + value). Tagged pointers distinguish them: bit 0 is 1 for
leaves, 0 for inner nodes.

```cpp
struct hamt_inner {
    std::atomic<int> rc{1};   // intrusive refcount
    uint32_t bitmap{0};       // which of 32 children are present

    uintptr_t* children() {   // variable-length array after struct
        return reinterpret_cast<uintptr_t*>(this + 1);
    }
    int child_count() const {
        return __builtin_popcount(bitmap);
    }
};

struct hamt_leaf {
    std::atomic<int> rc{1};
    uint64_t key;
    std::any value;
};
```

### 3.2 Lookup (BLR-free)

The read path is critical: dynamic variables may be read on every
function call (trace IDs, configuration, authentication tokens). The
`hamt_get` function compiles to pure inline arithmetic — no function
pointers, no virtual dispatch, no indirect calls:

```cpp
inline const std::any* hamt_get(uintptr_t root, uint64_t key) {
    uintptr_t node = root;
    int shift = 0;
    while (node && !hamt_is_leaf(node)) {
        auto* inner = hamt_to_inner(node);
        uint32_t idx = (key >> shift) & 0x1f;     // 5-bit index
        uint32_t bit = 1u << idx;
        if (!(inner->bitmap & bit)) return nullptr; // not present
        int pos = __builtin_popcount(inner->bitmap & (bit - 1));
        node = inner->children()[pos];
        shift += 5;
    }
    if (!node) return nullptr;
    auto* leaf = hamt_to_leaf(node);
    return (leaf->key == key) ? &leaf->value : nullptr;
}
```

At each level: extract 5 bits, check the bitmap, compute the
compressed index with `popcount`, follow the pointer. The entire path
is a sequence of shifts, masks, popcount, and loads. On ARM64,
`__builtin_popcount` compiles to the single-cycle `CNT` instruction.

### 3.3 Write (path-copying)

Writing to a dynamic variable does not mutate the existing tree.
Instead, `hamt_assoc` creates a new path from the root to the
modified leaf, sharing all unchanged subtrees with the original:

```
Before write (key K):
    Root₁ → [A, B, C]
                 ↓
                 B → [D, E]
                         ↓
                         E → leaf(K, old_value)

After write (key K, new_value):
    Root₂ → [A, B', C]   (Root₂ is new; A and C are shared)
                  ↓
                  B' → [D, E']  (B' is new; D is shared)
                          ↓
                          E' → leaf(K, new_value)  (new leaf)
```

Root₁ still exists, pointing to the old tree. Anyone holding a
reference to Root₁ sees the old value. Anyone holding Root₂ sees the
new value. The shared subtrees (A, C, D) have their reference counts
incremented but are not copied.

This is the source of copy-on-write isolation: a child imp
inherits the parent's root pointer (with a reference count bump).
If the child writes to a dynamic variable, it gets a new root; the
parent's root is unchanged.

### 3.4 Reference counting

Nodes use intrusive atomic reference counts (`std::atomic<int> rc` as
the first member of both inner and leaf nodes). Retain increments with
relaxed ordering (no memory fence needed — the caller already has a
valid reference). Release decrements and, on reaching zero,
recursively releases children before freeing the node.

The placement of `rc` as the first member of both node types allows
`hamt_retain` to work on tagged pointers by simply masking off the
tag bit:

```cpp
inline void hamt_retain(uintptr_t p) {
    reinterpret_cast<std::atomic<int>*>(p & ~uintptr_t(1))
        ->fetch_add(1, std::memory_order_relaxed);
}
```

## 4. Integration with imps

Each `Imp` struct has a `dyn_ctx_` field holding the HAMT root
(0 for empty). When `spawn()` creates a child, the child's `dyn_ctx_`
is initialised from the parent's with a retain:

```cpp
// In spawn():
child->dyn_ctx_ = parent->dyn_ctx_;
if (child->dyn_ctx_) hamt_retain(child->dyn_ctx_);
```

This is the inheritance mechanism. The child sees all of the parent's
dynamic bindings. If neither parent nor child writes, they share the
same tree with no copying — just a reference count bump.

When either writes, `hamt_assoc` creates a new root for the writer.
The other's root is unchanged. This is the isolation mechanism.

## 5. The public API

### `dynamic<T>`

A typed dynamic-scoped variable. Each instance gets a unique key
(monotonically increasing atomic counter):

```cpp
csp::dynamic<int> trace_level;
csp::dynamic<std::string> request_id{"unknown"};
```

Read with `operator*`, write with `operator=`:

```cpp
trace_level = 3;                    // write: path-copy HAMT
std::cout << *trace_level << "\n";  // read: HAMT lookup + any_cast
```

### `context`

A copyable snapshot of a HAMT root. Can be sent over channels:

```cpp
auto ctx = csp::context::current();  // snapshot
w << ctx;                            // send over channel

// In receiver:
csp::context ctx;
r >> ctx;
csp::context_scope scope(ctx);       // install foreign context
// dynamic variables now reflect the sender's bindings
```

This enables patterns like request context propagation: a handler
snapshots its context and sends it to a worker pool, where the worker
installs the context before processing.

### `context_scope`

RAII guard that saves the current context and restores it on
destruction:

```cpp
{
    csp::context_scope guard;   // save current context
    trace_level = 5;            // modify (path-copy)
    call_function();            // sees trace_level = 5
}
// trace_level restored to previous value
```

## 6. Performance characteristics

**Reads** are O(log₃₂ N) where N is the number of bindings. For
typical use (10-100 bindings), this is 1-2 levels of tree traversal.
The `hamt_get` function is inline and BLR-free, suitable for
hot-path access.

**Writes** are O(log₃₂ N) — they path-copy from root to the
modified leaf, creating 1-2 new nodes and sharing the rest. The new
nodes are allocated with `new` (cold path, not inline).

**Spawn** is O(1) — a reference count increment on the root.

**Destruction** is O(N) in the worst case (decrementing through the
entire tree), but amortised across shared owners, the cost is
typically much lower.

The `std::any` value type adds one level of indirection and a type
check on read (`std::any_cast`). For types that fit in `std::any`'s
small buffer (typically up to 3 pointers), this is an inline copy
with no heap allocation.

## 7. Comparison with alternatives

**Go**: No equivalent. Goroutines cannot carry implicit context;
`context.Context` must be passed explicitly as a function argument.
This is verbose but explicit.

**Java**: `InheritableThreadLocal` provides inherited thread-local
storage, but it copies the entire map on thread creation and does
not support copy-on-write isolation.

**Clojure**: `binding` provides true dynamic scoping with
per-thread binding stacks. The closest analogue to CSP's model, but
implemented with thread-local binding frames rather than a persistent
data structure.

CSP's approach is distinctive in using a persistent functional data
structure (the HAMT) to achieve inheritance, isolation, and snapshot
semantics simultaneously. The persistent structure means that
inheritance is free (refcount bump), isolation is automatic
(path-copy on write), and snapshots are first-class values that can
be sent over channels and installed in other imps — a
capability that none of the alternatives offer.
