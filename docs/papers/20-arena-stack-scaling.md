# Paper 20: Arena-Based Stack Scaling for 100K+ Imps

## Problem

Each imp currently gets its own `mmap`'d region (1 MB default) with a hardware
guard page at the bottom. On Linux, every `mmap` call consumes one entry in the
kernel's Virtual Memory Area (VMA) table, capped at `vm.max_map_count`
(typically 65530). Two VMAs per imp (the stack region + the guard page
`mprotect` split) means the system hits the limit around 32K simultaneous imps,
and at best ~64K before the next `mmap` call fails.

## Actors

1. **`StackPool::mmap_new()`** — allocates a new VMA via `mmap`.
2. **`StackPool::allocate()`** — serves a region from the free-list, or calls
   `mmap_new()`.
3. **`internal::spawn()`** — calls `allocate()`, places `Imp` at the top of the
   stack region, calls `make_fcontext`.
4. **`StackPool::release()`** — returns a region to the free-list via
   `madvise(MADV_FREE)`.
5. **`StackPool::maybe_shrink()`** — hints to the kernel to reclaim uncommitted
   pages below the current SP.

## Root Cause

Each stack region is a separate VMA. On Linux, `vm.max_map_count` is a kernel
limit on the total number of VMAs a process can have. Since mprotect on the
guard page creates a VMA split (one mapping is split into three: the guard
region, the usable region, and anything above), the VMA count is approximately
3× the number of live imps, not 1×.

## Invariant Being Violated

"The number of VMAs used by the process must remain below
`vm.max_map_count`."

With 100K concurrent imps, we need at least 200K–300K VMAs just for stacks,
but `vm.max_map_count` defaults to 65530.

## Approach: Arena Allocation

Allocate one large contiguous `mmap`'d region (an *arena*), then sub-divide it
into fixed-size stack slots in software. One arena = one VMA. With 100K imps at
64 KB per stack, the arena is 6.4 GB of virtual address space — large but fine
on 64-bit systems.

### Key Design Decisions

1. **Stack size in arena mode**: 64 KB per stack (configurable via
   `CSP_ARENA_STACK_SIZE`). Smaller than the 1 MB mmap stacks because (a) most
   imps are I/O-bound and use modest stack depth, and (b) the stack analysis
   tool (🎯T3.4) can verify actual usage. Keep 1 MB as the "non-arena" fallback.

2. **No hardware guard pages**: Without per-slot `mprotect`, there are no
   hardware guard pages. Stack overflow silently corrupts the adjacent imp's
   stack instead of segfaulting. We must add software overflow detection.

3. **Software overflow detection**: Store a `stack_overflow_limit` pointer in
   `Imp`, pointing to a red-zone N bytes above the bottom of the stack slot.
   At every CSP API checkpoint that suspends (yield, channel ops, spawn), check
   `CSP_FRAME_ADDRESS() >= stack_overflow_limit` and abort with a descriptive
   message if violated.

   The check is cheap (one comparison) and the checkpoints already exist
   (the `maybe_shrink` call sites). We add the overflow check alongside them.

4. **Arena growth**: Arenas are fixed-size slabs allocated on demand. A second
   arena is created when the first is exhausted. This amortises the VMA cost
   while keeping individual allocation O(1).

5. **Sanitizer fallback**: ASan and TSan shadow memory scales with VA range.
   A 6.4 GB arena would blow up shadow memory. The arena path is disabled under
   sanitizers (`CSP_USE_VM_STACKS == 0`), which already heap-allocate stacks.

### Arena Layout

```
[Arena base]
  slot 0: [guard zone (4KB, never written)] [usable (60KB)]
  slot 1: [guard zone (4KB, never written)] [usable (60KB)]
  ...
  slot N-1: [guard zone (4KB, never written)] [usable (60KB)]
[Arena end]
```

The `Imp` is placed at the top of the usable region (highest address), matching
the existing layout. `stack_overflow_limit` = `slot_base + guard_zone_size`.

The guard zone is software-only: nothing prevents writes there, but we
check the SP against the limit at every suspend point before the overflow
reaches adjacent slots.

### Implementation Plan

1. Add `CSP_USE_ARENA_STACKS` macro (enabled on Unix non-sanitizer builds, same
   guard as `CSP_USE_VM_STACKS`).

2. Add `ArenaPool` class to `stack_pool.h`/`stack_pool.cc`:
   - `static constexpr size_t kArenaSlotSize = 64 << 10`
   - `static constexpr size_t kArenaSlotGuard = 4096`
   - `static constexpr size_t kArenaCapacity = 4096` (4096 slots × 64KB = 256MB
     per arena; multiple arenas can coexist)
   - `StackRegion allocate_arena()` — find or create an arena, return a slot
   - `void release_arena(StackRegion)` — return slot to free-list

3. `StackRegion::overflow_limit` field: add `char* overflow_limit` to
   `StackRegion` (or put it in `Imp`).

4. Modify `spawn()` to populate `imp->stack_overflow_limit_`.

5. Add `check_stack_overflow(Imp* self)` inline in `csp_internal.h` — called
   at existing `maybe_shrink` call sites.

6. Stress test: `test/stack_density.test.cc` — spawn 100K trivial imps,
   verify no crash and all complete.

### Relationship to 🎯T3.4 (Stack Analysis)

T3.4's ARM64 stack depth analysis gives us the actual max stack depth for a
given function. This synergises with arena allocation:
- If analysis says an imp function uses ≤16 KB, its slot only needs to be
  16 KB, not 64 KB. We could use variable-slot arenas.
- For now, T3.3 uses fixed 64 KB slots (conservative). T3.4 can later tighten
  this, multiplying imp density further.

The current T3.4 code path is in `spawn()` (the sanitizer branch uses
`stack_analysis` to right-size heap stacks). Arena mode extends this:
once T3.4 is complete and accurate, arena slot size can be computed per
function rather than fixed.

### Anticipated Conflicts

- `CSP_ASAN` / `CSP_TSAN` paths: already gated behind `CSP_USE_VM_STACKS == 0`,
  which arena mode does not touch. No conflict.
- Windows path: VirtualAlloc-based, entirely separate `#ifdef _WIN32` block.
  Arena is Unix-only. No conflict.
- `maybe_shrink`: arena mode doesn't need physical page reclaim (no guard page
  split), so `maybe_shrink` becomes a no-op for arena stacks. The overflow check
  replaces it.
- T3.4 stack analysis: T3.4 adds per-function stack depth queries. Arena mode
  can be extended later to use T3.4 results for variable-slot sizing, but the
  fixed-slot arena doesn't conflict with T3.4's current analysis path.
