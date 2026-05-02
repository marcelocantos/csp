#pragma once

#include <cstddef>
#include <mutex>
#include <vector>

// Sanitizer detection: under ASan/TSan, shadow memory scales with mapped VA,
// so we fall back to heap allocation (new[]/delete[]).
// Two-level #if avoids MSVC's traditional preprocessor evaluating
// __has_feature() even when defined(__has_feature) is false.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define CSP_USE_VM_STACKS 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define CSP_USE_VM_STACKS 0
#endif
#endif
#ifndef CSP_USE_VM_STACKS
#define CSP_USE_VM_STACKS 1
#endif

// Arena-based stack allocation: enabled on Unix non-sanitizer non-Windows
// builds.  One large mmap per arena; stacks are sub-slots within it.
// This avoids the Linux vm.max_map_count limit (~65K VMAs) that would be
// hit with per-imp mmap at 100K+ concurrent imps.
#if CSP_USE_VM_STACKS && !defined(_WIN32)
#define CSP_USE_ARENA_STACKS 1
#else
#define CSP_USE_ARENA_STACKS 0
#endif

namespace csp::detail {

struct StackRegion {
    void* base = nullptr;
    size_t total_size = 0;
    // Software overflow limit: lowest address the SP may touch.
    // Non-null only for arena-mode stacks (no hardware guard page).
    // Checked at every CSP API checkpoint via check_stack_overflow().
    void* overflow_limit = nullptr;
    explicit operator bool() const { return base != nullptr; }
};

class StackPool {
public:
    static StackPool& instance();

    // Allocate a stack region. Returns a pooled or fresh region.
    StackRegion allocate();

    // Return a stack to the pool. Reclaims physical pages via madvise
    // (non-arena) or marks the slot free (arena).
    void release(StackRegion region);

    // Reclaim unused stack pages below the current SP.
    // No-op for arena stacks (no per-page reclaim supported).
    void maybe_shrink(StackRegion const& region, void* current_sp);

    // Unmap all pooled stacks. Called during shutdown.
    void drain();

    size_t page_size() const { return page_size_; }

    // Returns the number of arena slabs currently allocated.
    // Each slab is one VMA, so 100K imps requires at most ~25 slabs.
    // Returns 0 on non-arena builds (Windows, sanitizers).
    size_t slab_count() const {
#if CSP_USE_ARENA_STACKS
        std::lock_guard<std::mutex> lk(mu_);
        return arena_slabs_.size();
#else
        return 0;
#endif
    }

private:
    StackPool();

#if CSP_USE_VM_STACKS && !CSP_USE_ARENA_STACKS
    StackRegion mmap_new();
    void munmap_region(StackRegion region);
#elif CSP_USE_ARENA_STACKS
    StackRegion arena_alloc();
    void arena_free(StackRegion region);
#endif

    size_t page_size_;
#if CSP_USE_VM_STACKS && !CSP_USE_ARENA_STACKS
    size_t stack_size_;     // total VM region size (guard + usable)
#endif

    mutable std::mutex mu_;
    std::vector<StackRegion> free_list_;

    static constexpr size_t kDefaultStackSize = 1 << 20;  // 1MB (non-arena)
    static constexpr size_t kMaxPooled = 256;

#if CSP_USE_ARENA_STACKS
    // Arena slab parameters:
    // Each slab is one large mmap covering kArenaSlotsPerSlab stack slots.
    // Slot layout (low->high): [guard zone][usable region].
    // Imp is placed at the top of the usable region (highest address).
    // One slab = one VMA, so 100K imps needs at most ~25 slabs.
    static constexpr size_t kArenaSlotGuard  = 4096;          // 4KB software guard zone
    static constexpr size_t kArenaSlotUsable = 124 << 10;     // 124KB usable stack (QUIC/TLS needs >60KB)
    static constexpr size_t kArenaSlotSize   = kArenaSlotGuard + kArenaSlotUsable;  // 128KB/slot
    static constexpr size_t kArenaSlotsPerSlab = 4096;         // slots per slab
    static constexpr size_t kArenaSlabSize = kArenaSlotSize * kArenaSlotsPerSlab;   // 512MB per slab

    struct ArenaSlab {
        void* base = nullptr;
        size_t size = 0;
    };

    std::vector<ArenaSlab> arena_slabs_;  // all allocated slabs (for drain)
    // free_list_ holds arena StackRegions (with overflow_limit set)
#endif


public:
    // Initial committed region per stack on Windows (at the top, where RSP
    // starts).  The rest of the 1 MB virtual region is MEM_RESERVE, committed
    // on demand by the VEH handler.  Exposed here so spawn() can pass this
    // value to make_fcontext, which stores it as NT_TIB StackLimit.
    static constexpr size_t kInitialCommitSize = 64 * 1024;  // 64 KB
};

} // namespace csp::detail
