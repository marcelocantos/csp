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

StackRegion StackPool::mmap_new() {
    void* base = VirtualAlloc(nullptr, stack_size_,
                              MEM_RESERVE | MEM_COMMIT,
                              PAGE_READWRITE);
    if (!base) {
        throw std::bad_alloc();
    }
    // Guard page at the bottom (lowest address).
    DWORD old_protect;
    if (!VirtualProtect(base, page_size_, PAGE_NOACCESS, &old_protect)) {
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
    // Decommit the usable area (above guard page) to release physical pages.
    // The pages are re-committed on next access (demand-paged).
    char* usable = static_cast<char*>(region.base) + page_size_;
    size_t usable_len = region.total_size - page_size_;
    VirtualFree(usable, usable_len, MEM_DECOMMIT);
    // Re-commit so the region is usable again when recycled from the pool.
    VirtualAlloc(usable, usable_len, MEM_COMMIT, PAGE_READWRITE);

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
        VirtualFree(usable, reclaimable, MEM_DECOMMIT);
        VirtualAlloc(usable, reclaimable, MEM_COMMIT, PAGE_READWRITE);
    }
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
