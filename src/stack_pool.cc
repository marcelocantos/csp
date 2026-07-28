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

// One arena implementation for both slot classes (🎯T49; formerly a
// duplicated per-class pair). Geometry comes from kArenaGeometry[cls];
// mutable state (slabs for drain, free list) from arenas_[cls]. The
// Small class (🎯T3.4.1) is the same layout with a tighter slot —
// spawn() opts into it when the stack analyser confirms the imp fits.
StackRegion StackPool::arena_alloc(StackClass cls) {
    auto const& g = kArenaGeometry[static_cast<size_t>(cls)];
    auto& a = arenas_[static_cast<size_t>(cls)];
    std::lock_guard<std::mutex> lk(mu_);

    if (cls == StackClass::Small) {
        small_allocations_.fetch_add(1, std::memory_order_relaxed);
    }

    // Serve from the class free list first.
    if (!a.free_list.empty()) {
        auto region = a.free_list.back();
        a.free_list.pop_back();
        return region;
    }

    // Allocate a new slab and carve all slots into the free list.
    int flags = MAP_ANON | MAP_PRIVATE;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    void* base = mmap(nullptr, g.slab_size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (base == MAP_FAILED) {
        throw std::bad_alloc();
    }

    a.slabs.push_back({base, g.slab_size});

    // Carve the slab into slot StackRegions.
    // Return slot 0 directly; push slots 1..N-1 onto the free list.
    auto* p = static_cast<char*>(base);
    StackRegion first{};
    for (size_t i = 0; i < g.slots_per_slab; ++i) {
        StackRegion r;
        r.base = p;
        r.total_size = g.slot_size;
        r.overflow_limit = p + g.slot_guard;
        r.cls = cls;
        p += g.slot_size;
        if (i == 0) {
            first = r;
        } else {
            a.free_list.push_back(r);
        }
    }
    return first;
}

void StackPool::arena_free(StackRegion region) {
    auto const& g = kArenaGeometry[static_cast<size_t>(region.cls)];
    // Hint to the kernel that the usable pages can be reclaimed.
    char* usable = static_cast<char*>(region.base) + g.slot_guard;
    size_t usable_len = region.total_size - g.slot_guard;
#ifdef MADV_FREE
    madvise(usable, usable_len, MADV_FREE);
#else
    madvise(usable, usable_len, MADV_DONTNEED);
#endif

    std::lock_guard<std::mutex> lk(mu_);
    arenas_[static_cast<size_t>(region.cls)].free_list.push_back(region);
}

StackRegion StackPool::allocate(StackClass cls) {
    return arena_alloc(cls);
}

void StackPool::release(StackRegion region) {
    arena_free(region);
}

// maybe_shrink: inline no-op in the header for arena builds.

void StackPool::drain() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& a : arenas_) {
        a.free_list.clear();
        for (auto& slab : a.slabs) {
            munmap(slab.base, slab.size);
        }
        a.slabs.clear();
    }
}

// ============================================================
// mmap per-stack — Windows only (non-sanitizer)
// ============================================================
#elif CSP_USE_VM_STACKS

// This branch is Windows-only by construction: CSP_USE_ARENA_STACKS is
// defined as CSP_USE_VM_STACKS && !_WIN32 (stack_pool.h), so VM &&
// !ARENA implies _WIN32. The former #else Unix per-stack mmap
// implementation here was unreachable and has been removed (🎯T49).
#ifndef _WIN32
#error "CSP_USE_VM_STACKS without CSP_USE_ARENA_STACKS implies _WIN32"
#endif

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
