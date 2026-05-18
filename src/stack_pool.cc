// Copyright 2025 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
#include <csp/internal/stack_pool.h>

#if CSP_USE_VM_STACKS || CSP_USE_ARENA_STACKS
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#endif

#include <cassert>
#include <new>

namespace csp::detail {

StackPool& StackPool::instance() {
    static StackPool pool;
    return pool;
}

StackPool::StackPool()
    : page_size_(
#if CSP_USE_VM_STACKS || CSP_USE_ARENA_STACKS
#ifdef _WIN32
        [] {
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            return static_cast<size_t>(si.dwPageSize);
        }()
#else
        static_cast<size_t>(getpagesize())
#endif
#else
        4096
#endif
    )
#if CSP_USE_VM_STACKS && !CSP_USE_ARENA_STACKS
    , stack_size_(kDefaultStackSize)
#endif
{}

// ============================================================
// Arena-based allocation (Unix non-sanitizer non-Windows)
// ============================================================
#if CSP_USE_ARENA_STACKS

StackRegion StackPool::arena_alloc() {
    std::lock_guard<std::mutex> lk(mu_);

    // Serve from free list first.
    if (!free_list_.empty()) {
        auto region = free_list_.back();
        free_list_.pop_back();
        return region;
    }

    // Allocate a new slab and carve all slots into the free list.
    int flags = MAP_ANON | MAP_PRIVATE;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    void* base = mmap(nullptr, kArenaSlabSize, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (base == MAP_FAILED) {
        throw std::bad_alloc();
    }

    arena_slabs_.push_back({base, kArenaSlabSize});

    // Carve the slab into slot StackRegions.
    // Return slot 0 directly; push slots 1..N-1 onto the free list.
    auto* p = static_cast<char*>(base);
    StackRegion first{};
    for (size_t i = 0; i < kArenaSlotsPerSlab; ++i) {
        StackRegion r;
        r.base = p;
        r.total_size = kArenaSlotSize;
        r.overflow_limit = p + kArenaSlotGuard;
        r.cls = StackClass::Default;
        p += kArenaSlotSize;
        if (i == 0) {
            first = r;
        } else {
            free_list_.push_back(r);
        }
    }
    return first;
}

void StackPool::arena_free(StackRegion region) {
    // Hint to the kernel that the usable pages can be reclaimed.
    char* usable = static_cast<char*>(region.base) + kArenaSlotGuard;
    size_t usable_len = region.total_size - kArenaSlotGuard;
#ifdef MADV_FREE
    madvise(usable, usable_len, MADV_FREE);
#else
    madvise(usable, usable_len, MADV_DONTNEED);
#endif

    std::lock_guard<std::mutex> lk(mu_);
    free_list_.push_back(region);
}

// Small-class arena (🎯T3.4.1): same layout as the default arena, with a
// tighter slot and separate free list / slab vector. spawn() opts into this
// class when the stack analyser confirms the imp fits.
StackRegion StackPool::arena_alloc_small() {
    std::lock_guard<std::mutex> lk(mu_);

    if (!small_free_list_.empty()) {
        auto region = small_free_list_.back();
        small_free_list_.pop_back();
        small_allocations_.fetch_add(1, std::memory_order_relaxed);
        return region;
    }

    int flags = MAP_ANON | MAP_PRIVATE;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    void* base = mmap(nullptr, kArenaSmallSlabSize, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (base == MAP_FAILED) {
        throw std::bad_alloc();
    }

    small_arena_slabs_.push_back({base, kArenaSmallSlabSize});

    auto* p = static_cast<char*>(base);
    StackRegion first{};
    for (size_t i = 0; i < kArenaSmallSlotsPerSlab; ++i) {
        StackRegion r;
        r.base = p;
        r.total_size = kArenaSmallSlotSize;
        r.overflow_limit = p + kArenaSmallSlotGuard;
        r.cls = StackClass::Small;
        p += kArenaSmallSlotSize;
        if (i == 0) {
            first = r;
        } else {
            small_free_list_.push_back(r);
        }
    }
    small_allocations_.fetch_add(1, std::memory_order_relaxed);
    return first;
}

void StackPool::arena_free_small(StackRegion region) {
    char* usable = static_cast<char*>(region.base) + kArenaSmallSlotGuard;
    size_t usable_len = region.total_size - kArenaSmallSlotGuard;
#ifdef MADV_FREE
    madvise(usable, usable_len, MADV_FREE);
#else
    madvise(usable, usable_len, MADV_DONTNEED);
#endif

    std::lock_guard<std::mutex> lk(mu_);
    small_free_list_.push_back(region);
}

StackRegion StackPool::allocate(StackClass cls) {
    if (cls == StackClass::Small) return arena_alloc_small();
    return arena_alloc();
}

void StackPool::release(StackRegion region) {
    if (region.cls == StackClass::Small) {
        arena_free_small(region);
    } else {
        arena_free(region);
    }
}

void StackPool::maybe_shrink(StackRegion const&, void*) {
    // No-op for arena stacks: the entire slab is one VMA; we cannot reclaim
    // individual slot pages without splitting the mapping. The software
    // overflow limit (overflow_limit) replaces guard-page overflow detection.
}

void StackPool::drain() {
    std::lock_guard<std::mutex> lk(mu_);
    free_list_.clear();
    small_free_list_.clear();
    for (auto& slab : arena_slabs_) {
        munmap(slab.base, slab.size);
    }
    arena_slabs_.clear();
    for (auto& slab : small_arena_slabs_) {
        munmap(slab.base, slab.size);
    }
    small_arena_slabs_.clear();
}

// ============================================================
// mmap per-stack — Windows only (non-sanitizer)
// ============================================================
#elif CSP_USE_VM_STACKS

#ifdef _WIN32

// --- Windows: VirtualAlloc-based stack regions ---
//
// Reserve 1MB of virtual address space per stack but only commit a small
// initial portion at the top (where RSP starts). A Vectored Exception
// Handler (VEH) commits pages on demand when the stack grows, replicating
// the demand-paged behavior of mmap on Unix. This keeps the commit charge
// proportional to actual stack usage, not the number of live imps.

static constexpr size_t kInitialCommit = StackPool::kInitialCommitSize;

static LONG WINAPI stack_guard_handler(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    auto fault_addr = reinterpret_cast<void*>(
        ep->ExceptionRecord->ExceptionInformation[1]);

    // Only commit pages that are in MEM_RESERVE state (reserved but not
    // yet committed).  This prevents accidentally changing the protection
    // of already-committed pages (guard pages, read-only pages, etc.)
    // which would mask real crashes.
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(fault_addr, &mbi, sizeof(mbi)))
        return EXCEPTION_CONTINUE_SEARCH;
    if (mbi.State != MEM_RESERVE)
        return EXCEPTION_CONTINUE_SEARCH;

    auto& pool = StackPool::instance();
    size_t page = pool.page_size();

    // Align fault address down to page boundary.
    auto page_base = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(fault_addr) & ~(page - 1));

    void* result = VirtualAlloc(page_base, page, MEM_COMMIT, PAGE_READWRITE);
    if (result) {
        // Keep NT_TIB StackLimit in sync with the committed boundary.
        // jump_fcontext sets StackLimit from the saved context on each
        // switch; if a demand-committed page extends below the current
        // StackLimit, update the TEB so MSVC's C++ exception dispatch
        // (which probes the stack using StackLimit) doesn't fault on
        // uncommitted pages -- a nested ACCESS_VIOLATION during dispatch
        // terminates the process without VEH notification.
        auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
        if (page_base < tib->StackLimit) {
            tib->StackLimit = page_base;
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

static std::once_flag g_veh_once;

StackRegion StackPool::mmap_new() {
    // Install the demand-commit VEH once.  Use priority 0 (end of chain)
    // so the diagnostic VEH in test/main.cc (registered with priority 1)
    // always sees exceptions first for logging.
    std::call_once(g_veh_once, [] {
        AddVectoredExceptionHandler(0, stack_guard_handler);
    });

    // Reserve address space without committing physical pages.
    void* base = VirtualAlloc(nullptr, stack_size_,
                              MEM_RESERVE,
                              PAGE_NOACCESS);
    if (!base) {
        throw std::bad_alloc();
    }

    // Commit guard page at the bottom (PAGE_NOACCESS after commit -- use
    // PAGE_READWRITE then protect, since MEM_COMMIT requires valid protection).
    void* guard = VirtualAlloc(base, page_size_,
                               MEM_COMMIT, PAGE_READWRITE);
    if (!guard) {
        VirtualFree(base, 0, MEM_RELEASE);
        throw std::bad_alloc();
    }
    DWORD old_protect;
    VirtualProtect(base, page_size_, PAGE_NOACCESS, &old_protect);

    // Commit the initial region at the top of the stack (where RSP starts).
    char* top = static_cast<char*>(base) + stack_size_;
    size_t commit = (kInitialCommit < stack_size_ - page_size_)
                        ? kInitialCommit
                        : stack_size_ - page_size_;
    void* committed = VirtualAlloc(top - commit, commit,
                                   MEM_COMMIT, PAGE_READWRITE);
    if (!committed) {
        VirtualFree(base, 0, MEM_RELEASE);
        throw std::bad_alloc();
    }

    return {base, stack_size_, nullptr};
}

void StackPool::munmap_region(StackRegion region) {
    VirtualFree(region.base, 0, MEM_RELEASE);
}

StackRegion StackPool::allocate(StackClass /*cls*/) {
    // Windows VM mode: no distinct small slot — demand-commit makes the 1 MB
    // MEM_RESERVE region effectively cheap, so the slot-class hint is
    // ignored here. The Small caller still benefits from analyser-driven
    // gating on arena builds.
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!free_list_.empty()) {
            auto region = free_list_.back();
            free_list_.pop_back();
            return region;
        }
    }
    return mmap_new();
}

void StackPool::release(StackRegion region) {
    // Decommit everything except the guard page to release physical pages.
    char* usable = static_cast<char*>(region.base) + page_size_;
    size_t usable_len = region.total_size - page_size_;
    VirtualFree(usable, usable_len, MEM_DECOMMIT);

    // Re-commit only the initial portion at the top so the stack is
    // usable when recycled from the pool. The VEH handles further growth.
    char* top = static_cast<char*>(region.base) + region.total_size;
    size_t commit = (kInitialCommit < usable_len) ? kInitialCommit : usable_len;
    VirtualAlloc(top - commit, commit, MEM_COMMIT, PAGE_READWRITE);

    std::lock_guard<std::mutex> lk(mu_);
    if (free_list_.size() < kMaxPooled) {
        free_list_.push_back(region);
    } else {
        munmap_region(region);
    }
}

void StackPool::maybe_shrink(StackRegion const&, void*) {
    // No-op on Windows.  Decommitting pages (MEM_DECOMMIT) and raising
    // StackLimit is unsafe because MSVC's C++ exception dispatch
    // (RtlDispatchException -> RtlVirtualUnwind) runs on the current
    // stack.  If the dispatch needs more stack than the headroom between
    // RSP and StackLimit, it faults on uncommitted pages.  That nested
    // ACCESS_VIOLATION during exception dispatch is a double-fault that
    // terminates the process without VEH notification -- our
    // demand-commit handler never gets a chance to commit the page.
    //
    // Pages stay committed until the stack is released to the pool,
    // where release() decommits everything and re-commits only the
    // initial region.
}

void StackPool::drain() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& r : free_list_) {
        munmap_region(r);
    }
    free_list_.clear();
}

#else // !_WIN32: Unix per-stack mmap (unreachable: CSP_USE_ARENA_STACKS always
      // takes priority on non-Windows Unix non-sanitizer builds)

static int madv_free_flag() {
#ifdef MADV_FREE
    return MADV_FREE;
#else
    return MADV_DONTNEED;
#endif
}

StackRegion StackPool::mmap_new() {
    int flags = MAP_ANON | MAP_PRIVATE;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    void* base = mmap(nullptr, stack_size_, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (base == MAP_FAILED) {
        throw std::bad_alloc();
    }
    if (mprotect(base, page_size_, PROT_NONE) != 0) {
        munmap(base, stack_size_);
        throw std::bad_alloc();
    }
    return {base, stack_size_, nullptr};
}

void StackPool::munmap_region(StackRegion region) {
    munmap(region.base, region.total_size);
}

StackRegion StackPool::allocate(StackClass /*cls*/) {
    // Per-stack mmap mode (unreachable in practice — arena takes priority on
    // non-Windows Unix builds). Slot-class hint ignored.
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!free_list_.empty()) {
            auto region = free_list_.back();
            free_list_.pop_back();
            return region;
        }
    }
    return mmap_new();
}

void StackPool::release(StackRegion region) {
    char* usable = static_cast<char*>(region.base) + page_size_;
    size_t usable_len = region.total_size - page_size_;
    madvise(usable, usable_len, madv_free_flag());

    std::lock_guard<std::mutex> lk(mu_);
    if (free_list_.size() < kMaxPooled) {
        free_list_.push_back(region);
    } else {
        munmap_region(region);
    }
}

void StackPool::maybe_shrink(StackRegion const& region, void* current_sp) {
    char* usable = static_cast<char*>(region.base) + page_size_;
    auto sp_val = reinterpret_cast<uintptr_t>(current_sp);
    char* sp_page = reinterpret_cast<char*>(sp_val & ~(page_size_ - 1));
    char* shrink_to = sp_page - 2 * page_size_;
    if (shrink_to > usable + static_cast<ptrdiff_t>(page_size_)) {
        size_t reclaimable = static_cast<size_t>(shrink_to - usable);
        madvise(usable, reclaimable, madv_free_flag());
    }
}

void StackPool::drain() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& r : free_list_) {
        munmap_region(r);
    }
    free_list_.clear();
}

#endif // _WIN32

#else // !CSP_USE_ARENA_STACKS && !CSP_USE_VM_STACKS -- sanitizer heap fallback

struct alignas(16) StackSlotAlloc { char c[16]; };

StackRegion StackPool::allocate(StackClass /*cls*/) {
    // Under sanitizers: heap-allocate stacks. Sanitizer instrumentation
    // already adds significant overhead; the analyser-driven small-slot
    // path is not exercised here, so the cls hint is ignored.
    static constexpr size_t kSanitStack = 128 << 10;
    static constexpr size_t S = kSanitStack / 16;
    auto* stk = new StackSlotAlloc[S];
    return {stk, kSanitStack, nullptr};
}

void StackPool::release(StackRegion region) {
    delete[] static_cast<StackSlotAlloc*>(region.base);
}

void StackPool::maybe_shrink(StackRegion const&, void*) {
    // No-op under sanitizers.
}

void StackPool::drain() {
    // Nothing pooled under sanitizers.
}

#endif // CSP_USE_ARENA_STACKS / CSP_USE_VM_STACKS

} // namespace csp::detail
