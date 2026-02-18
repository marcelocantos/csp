#include <csp/internal/stack_pool.h>

#if CSP_USE_MMAP_STACKS
#include <sys/mman.h>
#include <unistd.h>
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
#if CSP_USE_MMAP_STACKS
        static_cast<size_t>(getpagesize())
#else
        4096
#endif
    )
    , stack_size_(kDefaultStackSize)
{}

#if CSP_USE_MMAP_STACKS

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

#else // !CSP_USE_MMAP_STACKS — sanitizer fallback

struct alignas(16) StackSlotAlloc { char c[16]; };

StackRegion StackPool::mmap_new() {
    // Not used under sanitizers.
    return {};
}

void StackPool::munmap_region(StackRegion region) {
    delete[] static_cast<StackSlotAlloc*>(region.base);
}

StackRegion StackPool::allocate() {
    // Under sanitizers: use the fixed stack size from Microthread.
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

#endif // CSP_USE_MMAP_STACKS

} // namespace csp::detail
