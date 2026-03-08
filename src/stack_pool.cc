#include <csp/internal/stack_pool.h>

#if CSP_USE_VM_STACKS
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
#if CSP_USE_VM_STACKS
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
#if CSP_USE_VM_STACKS
    , stack_size_(kDefaultStackSize)
#endif
{}

#if CSP_USE_VM_STACKS

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
        // uncommitted pages — a nested ACCESS_VIOLATION during dispatch
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

    // Commit guard page at the bottom (PAGE_NOACCESS after commit — use
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

    return {base, stack_size_};
}

void StackPool::munmap_region(StackRegion region) {
    VirtualFree(region.base, 0, MEM_RELEASE);
}

StackRegion StackPool::allocate() {
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
    // (RtlDispatchException → RtlVirtualUnwind) runs on the current
    // stack.  If the dispatch needs more stack than the headroom between
    // RSP and StackLimit, it faults on uncommitted pages.  That nested
    // ACCESS_VIOLATION during exception dispatch is a double-fault that
    // terminates the process without VEH notification — our
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

#else // !_WIN32

// --- Unix: mmap-based stack regions ---

StackRegion StackPool::mmap_new() {
    int flags = MAP_ANON | MAP_PRIVATE;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    void* base = mmap(nullptr, stack_size_, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (base == MAP_FAILED) {
        throw std::bad_alloc();
    }
    // Guard page at the bottom (lowest address).
    if (mprotect(base, page_size_, PROT_NONE) != 0) {
        munmap(base, stack_size_);
        throw std::bad_alloc();
    }
    return {base, stack_size_};
}

void StackPool::munmap_region(StackRegion region) {
    munmap(region.base, region.total_size);
}

static int madv_free_flag() {
#ifdef MADV_FREE
    return MADV_FREE;
#else
    return MADV_DONTNEED;
#endif
}

StackRegion StackPool::allocate() {
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
    // MADV_FREE the usable area (above guard page). The kernel reclaims
    // these pages lazily — if they're reused before reclamation, no
    // re-fault cost.
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
    // Round SP down to page boundary.
    auto sp_val = reinterpret_cast<uintptr_t>(current_sp);
    char* sp_page = reinterpret_cast<char*>(sp_val & ~(page_size_ - 1));
    // Keep 2 pages of headroom below SP to avoid thrashing.
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

#else // !CSP_USE_VM_STACKS — sanitizer fallback

struct alignas(16) StackSlotAlloc { char c[16]; };

StackRegion StackPool::mmap_new() {
    // Not used under sanitizers.
    return {};
}

void StackPool::munmap_region(StackRegion region) {
    delete[] static_cast<StackSlotAlloc*>(region.base);
}

StackRegion StackPool::allocate() {
    // Under sanitizers: use the fixed stack size from Imp.
    // 128KB under sanitizers = 8192 StackSlotAlloc (16 bytes each).
    static constexpr size_t kSanitStack = 128 << 10;
    static constexpr size_t S = kSanitStack / 16;
    auto* stk = new StackSlotAlloc[S];
    return {stk, kSanitStack};
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

#endif // CSP_USE_VM_STACKS

} // namespace csp::detail
