# A Million Threads on a Megabyte

## Abstract

Imp libraries face a stack allocation dilemma: too small and
functions overflow; too large and memory is wasted. We describe a
three-part solution: demand-paged virtual stacks that allocate only
the physical memory actually touched, a pool that recycles stack
regions across imp lifetimes, and an ARM64 instruction walker
that estimates each function's maximum stack depth at spawn time. The
instruction walker itself must avoid indirect function calls to
produce exact results — requiring custom hash maps that compile to
pure inline arithmetic. Together, these techniques allow thousands of
concurrent imps with minimal physical memory overhead.

## 1. The stack allocation problem

Every imp needs a stack. The question is how much.

A fixed allocation is always wrong. Too small, and a perfectly valid
function call chain overflows into the guard page, killing the
process. Too large, and a program with 10,000 imps consumes
gigabytes of memory for stacks that are 90% unused.

Go solves this with segmented stacks (now contiguous, with copying):
goroutines start with a small stack (a few KB) and grow dynamically
when a function prologue detects that the stack is too small. This
requires compiler cooperation — the prologue check is inserted at
every function entry.

CSP doesn't have compiler cooperation. It's a library, not a
language runtime. The C++ compiler doesn't know about imp
stacks and won't insert growth checks. We need a solution that works
within the standard C++ compilation model.

## 2. Demand-paged virtual stacks

The first part of the solution exploits virtual memory. Each
imp gets a 1 MB virtual address region, allocated via `mmap`
with `MAP_ANON | MAP_PRIVATE`:

```cpp
void* base = mmap(nullptr, kDefaultStackSize, // 1 MB
                  PROT_READ | PROT_WRITE,
                  MAP_ANON | MAP_PRIVATE, -1, 0);
mprotect(base, page_size, PROT_NONE);  // guard page at bottom
```

The guard page at the lowest address catches stack overflow — a
write below the stack limit triggers `SIGSEGV` rather than silently
corrupting adjacent memory.

The key insight: `mmap` reserves virtual address space but does not
allocate physical pages. The kernel faults in physical pages
on demand, one page at a time, as the stack grows downward. A
imp that uses only 4 KB of stack consumes exactly one physical
page (4 KB), not 1 MB.

This means the 1 MB reservation is free in practice. The cost is
virtual address space, which is abundant on 64-bit systems (256 TB on
ARM64 macOS). The physical cost is proportional to actual stack depth,
not to the reservation size.

## 3. The stack pool

Allocating and freeing `mmap` regions per imp is expensive —
each call is a kernel trap, and the kernel must update page tables.
For a scheduler that creates and destroys thousands of imps
per second, this overhead is significant.

The stack pool caches freed regions in a free list:

```cpp
StackRegion StackPool::allocate() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!free_list_.empty()) {
            auto region = free_list_.back();
            free_list_.pop_back();
            return region;
        }
    }
    return mmap_new();  // fallback: new mmap region
}
```

When an imp exits, its stack region is returned to the pool
rather than unmapped. The pool retains up to 256 regions. Beyond
that, excess regions are unmapped.

On release, the pool calls `MADV_FREE` on the usable area (above
the guard page):

```cpp
void StackPool::release(StackRegion region) {
    char* usable = static_cast<char*>(region.base) + page_size_;
    size_t usable_len = region.total_size - page_size_;
    madvise(usable, usable_len, madv_free_flag());

    std::lock_guard<std::mutex> lk(mu_);
    if (free_list_.size() < kMaxPooled)
        free_list_.push_back(region);
    else
        munmap_region(region);
}
```

`MADV_FREE` tells the kernel that the pages *may* be reclaimed but
doesn't require immediate action. If the region is reused before the
kernel reclaims the pages, no page fault occurs — the old physical
pages are still mapped. If memory pressure rises, the kernel reclaims
them silently. This gives the best of both worlds: fast reuse in the
common case, automatic memory return under pressure.

## 4. Shrinking live stacks

A imp that once made a deep call chain (perhaps during
initialisation) and then settles into a shallow loop retains the
physical pages from the deep phase. Over time, thousands of
imps accumulating high-water-mark pages can waste significant
memory.

The `maybe_shrink()` function, called at API boundaries (channel
operations, timer waits), reclaims unused pages below the current
stack pointer:

```cpp
void StackPool::maybe_shrink(StackRegion const& region, void* current_sp) {
    char* usable = static_cast<char*>(region.base) + page_size_;
    auto sp_val = reinterpret_cast<uintptr_t>(current_sp);
    char* sp_page = reinterpret_cast<char*>(sp_val & ~(page_size_ - 1));
    // Keep 2 pages of headroom below SP.
    char* shrink_to = sp_page - 2 * page_size_;
    if (shrink_to > usable + page_size_) {
        size_t reclaimable = static_cast<size_t>(shrink_to - usable);
        madvise(usable, reclaimable, madv_free_flag());
    }
}
```

The two-page headroom prevents thrashing: if the stack depth
oscillates around a page boundary, the headroom absorbs the
oscillation without triggering repeated `madvise` calls.

The choice of API boundaries as shrink points is deliberate. These
are moments when the imp is about to suspend (blocking on a
channel or sleeping on a timer), so the overhead of the `madvise`
syscall is amortised against the much larger cost of the context
switch.

## 5. The ARM64 instruction walker

The demand-paged stack handles the common case well, but for optimal
stack sizing, the library includes a static analysis pass: an ARM64
instruction walker that estimates the maximum stack depth of each
imp entry function.

The walker operates at spawn time. Starting from the entry function's
first instruction, it decodes ARM64 instructions one by one, tracking:

- **`sub sp, sp, #N`**: stack pointer decremented by N.
- **`stp` / `str` to `[sp, #offset]`**: stores to the stack frame
  (confirming the frame size).
- **`bl <target>`**: direct call — recursively analyse the target.
- **`blr <reg>`**: indirect call — cannot follow; fall back to
  conservative estimate.
- **`b <target>`**: unconditional branch — follow it.
- **`b.cond <target>`**: conditional branch — analyse both paths,
  take the maximum.
- **`ret`**: end of function.

The walker builds an expression tree representing the maximum stack
depth as `max(path1, path2, ...)` across all control flow paths,
then evaluates it to a single integer.

### 5.1 The bootstrapping problem

The walker itself uses data structures — hash maps for memoization,
sets for cycle detection, vectors for work lists. Under sanitizers,
STL containers generate `BLR` instructions for allocator dispatch and
virtual destructors. When the walker analyses its own code (which
happens on the first spawn), those BLRs make the result inexact.

The solution: write custom containers that compile to pure inline
arithmetic — no indirect calls, no virtual dispatch, no allocator
BLR:

```cpp
// Open-addressing hash map: pure inline arithmetic, zero BLR.
template <typename V>
class ptr_map {
    struct slot { const void* key; V value; bool occupied; };
    slot* data_;
    size_t cap_, count_;

    static size_t hash(const void* p) {
        return reinterpret_cast<uintptr_t>(p) * 0x9E3779B97F4A7C15ULL;
    }
    // ... grow(), find(), emplace() — all inline ...
};
```

The Fibonacci hash constant (`0x9E3779B97F4A7C15`) provides good
distribution. Open addressing with linear probing avoids linked-list
nodes and their associated allocator calls. The entire map compiles
to load, store, multiply, shift, and compare instructions — no
function pointers anywhere in the generated code.

Similarly, reference-counted pointers for the expression tree use an
intrusive scheme (refcount embedded in the object) with a direct
`delete` call in the destructor, replacing `shared_ptr`'s virtual
control block destructor.

### 5.2 System thread vs. imp spawns

Full analysis is recursive and can consume significant stack space
(the walker follows call chains transitively). Running it on a
imp's small stack would risk overflow — the analyzer
overflowing the stack it's trying to size.

The solution: only system-thread spawns (the initial `spawn()` calls
before the M:N scheduler starts) run full analysis. Imp
spawns — which happen on imp stacks — use a cache-only
lookup. If the function was previously analysed (during system-thread
bootstrap), the cached result is used. If not, the conservative
default applies.

In practice, most imp entry functions are lambda wrappers or
combinator bodies that are first spawned during initialisation,
populating the cache. Subsequent spawns within running imps
hit the cache and avoid the deep analysis.

## 6. Sanitizer fallback

Under AddressSanitizer or ThreadSanitizer, the demand-paged mmap
approach becomes impractical. These sanitizers allocate shadow memory
proportional to the virtual address space mapped — a 1 MB mmap
region generates ~250 KB of shadow memory (TSan) or ~128 KB (ASan).
With thousands of imps, the shadow memory alone can exhaust
the system.

The fallback is simple: under sanitizers, the stack pool allocates
128 KB heap regions (`new char[131072]`) instead of 1 MB mmap
regions. This is large enough for most imp entry functions
and small enough that shadow memory remains manageable.

The stack depth analyzer is also disabled under sanitizers (via `#if`
guard), since the BLR instructions injected by sanitizer
instrumentation would make every result inexact regardless of the
custom containers.

## 7. Putting it together

The three mechanisms compose:

1. **Demand-paged mmap** provides a large virtual address space (1 MB)
   with minimal physical cost (pages faulted on use).
2. **The stack pool** amortises mmap/munmap overhead across
   imp lifetimes.
3. **`maybe_shrink()`** reclaims physical pages from live stacks
   that have passed their high-water mark.
4. **The instruction walker** provides spawn-time stack depth
   estimates for sizing decisions and diagnostics.

A program spawning 10,000 imps with typical stack depths of
8-16 KB uses approximately 80-160 MB of physical memory for stacks,
despite reserving 10 GB of virtual address space. The 256-entry pool
handles burst creation without kernel traps. The per-API shrinking
keeps long-lived imps from accumulating dead pages.

The result is that stack management, typically one of the most
painful aspects of userspace threading, becomes effectively invisible
to the library user. Imps get stacks that are simultaneously
large enough to never overflow in practice and small enough to run
thousands concurrently on a laptop.
