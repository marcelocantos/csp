# Dynamic Scope Alternatives to HAMT

Status: parked. The current HAMT works and isn't a bottleneck. This
paper records design exploration for future reference.

## Current design

Persistent HAMT with intrusive atomic refcounting (231 lines across
`hamt.h` and `hamt.cc`). Paper 06 documents the design; paper 09
documents the spawn race bug (fixed). The HAMT provides O(log₃₂ N)
lookup, O(1) spawn (refcount bump), and copy-on-write isolation via
path-copying on write.

## Why change was considered

- Atomic refcounting on every HAMT node is more machinery than needed
  for typical usage (3-10 dynamic variables).
- The refcount lifecycle has produced real bugs (paper 09
  use-after-free in `start()`).
- The tagged-pointer / popcount / path-copy machinery is complex
  relative to the problem size.

## Alternatives explored

### 1. Chained stack arrays (original 🎯T14)

Each `csp::local` is a list node on the imp's fcontext stack. Entries
are inline in the node. Parent pointer forms a chain. Spawn coalesces
the chain into a flat heap-allocated array owned by the child.

**Pros**: Zero heap alloc on bind/unbind. No refcounting. No pointer
tagging.

**Rejected because**: Some dynamic scope values aren't safe to copy.
The coalesce-on-spawn step would invoke copy constructors (including
atomic refcount bumps on `shared_ptr`-wrapped non-copyable values), so
it's not a simple memcpy. This undermines the "no atomic ops" benefit.

### 2. Intrusive linked list of heap segments + shared_ptr

Each `csp::local` allocates one heap segment holding all its bindings.
Segments form a singly-linked list via `shared_ptr<segment> parent`.
The imp's `dyn_ctx_` is a `shared_ptr` to the head.

**Pros**: Spawn is a single refcount bump (on the head segment, not
per-node). Simpler than HAMT — no tree structure, no pointer tagging.

**Rejected because**: Still uses atomic refcounting (via `shared_ptr`).
Trading one refcounted structure for a simpler one is a lateral move,
not a fundamental improvement. The HAMT already works.

### 3. Garbage collection

Eliminate refcounting entirely. The runtime knows every live imp
(the scheduler tracks them), so the root set for dynamic scope is
enumerable: every `Imp::dyn_ctx_` pointer.

**Collection strategy**: Mark-sweep at quiescence points (all workers
parked, no imp running). The quiescence detection already exists for
`fake_clock`. Between collections, HAMT nodes are allocated normally
but never individually freed.

**The `context` problem**: `context` objects are user-visible HAMT
root handles that can escape into channels or user stacks. They
create HAMT references outside the runtime's control, breaking the
"root set = live imps" invariant.

**Solution — opaque handles**: `context::current()` returns an opaque
ID (not a pointer). The runtime maintains a handle table mapping IDs
to HAMT roots. User code never holds HAMT pointers. The root set
becomes: all live imps' `dyn_ctx_` + all live handle table entries —
both runtime-internal and enumerable.

Handle lifecycle: `context::current()` inserts into the table,
`~context` removes the entry, `context_scope` looks up by ID.

**Pros**: Zero atomic ops on the hot path. Spawn is a raw pointer
copy. `~local` just restores a pointer. The runtime fully controls
all HAMT references.

**Parked because**: Significant implementation complexity (collector,
handle table, quiescence integration) for a problem that isn't
currently causing issues.

### 4. Flat root node optimisation (compatible with any approach)

Since dynamic scope is usually small (3-10 bindings), size the HAMT
root node to hold entries inline without children. Lookup becomes a
linear scan of a small contiguous array. The tree structure exists
only as an overflow path that rarely triggers.

This is an incremental optimisation compatible with the existing HAMT
or any replacement. Worth considering independently if lookup
performance becomes relevant.

## Recommendation

Keep the HAMT. The bugs are fixed, the code is contained, and none of
the alternatives offer a compelling improvement relative to their
implementation cost. Revisit if:

- Profiling shows dynamic scope operations as a bottleneck
- A new HAMT-related bug surfaces
- The codebase moves to support non-copyable dynamic scope values
  (which would require rethinking the `std::any` value type regardless)
