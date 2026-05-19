#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
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

// Slot size class for stack allocation. Default is the conservative 124 KB
// slot used for any imp whose stack depth is unknown or large. Small is a
// tighter slot (≈16 KB usable) selected by spawn() when the stack analyser
// returns an exact estimate that fits with headroom (🎯T3.4.1). The choice
// is recorded on the StackRegion so release() can route back to the right
// free list.
enum class StackClass : uint8_t {
    Default = 0,
    Small   = 1,
};

struct StackRegion {
    void* base = nullptr;
    size_t total_size = 0;
    // Software overflow limit: lowest address the SP may touch.
    // Non-null only for arena-mode stacks (no hardware guard page).
    // Checked at every CSP API checkpoint via check_stack_overflow().
    void* overflow_limit = nullptr;
    StackClass cls = StackClass::Default;
    explicit operator bool() const { return base != nullptr; }
};

class StackPool {
public:
    static StackPool& instance();

    // Allocate a stack region. Returns a pooled or fresh region.
    // `cls == Small` returns a tight slot when the build supports two slot
    // classes (arena mode); otherwise falls back to the Default slot.
    StackRegion allocate(StackClass cls = StackClass::Default);

    // Return a stack to the pool. Reclaims physical pages via madvise
    // (non-arena) or marks the slot free (arena). The slot class is read
    // from region.cls.
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
        return arena_slabs_.size() + small_arena_slabs_.size();
#else
        return 0;
#endif
    }

    // Usable bytes available in a Small-class slot for the current build.
    // Returns 0 on builds that do not support distinct small slots — spawn()
    // uses this to gate the analyser-driven path so the check naturally
    // fails outside arena mode.
    static constexpr size_t small_slot_usable_bytes() {
#if CSP_USE_ARENA_STACKS
        return kArenaSmallSlotUsable;
#else
        return 0;
#endif
    }

    // Total allocations served from the Small free list since process start.
    // Lock-free read intended for tests and observability; not exact under
    // concurrent allocation. Returns 0 on builds without distinct small
    // slots.
    size_t small_allocations() const {
#if CSP_USE_ARENA_STACKS
        return small_allocations_.load(std::memory_order_relaxed);
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
    StackRegion arena_alloc_small();
    void arena_free_small(StackRegion region);
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

    // Small slot parameters (🎯T3.4.1): selected by spawn() when the stack
    // analyser returns is_exact with depth + headroom that fits here. Same
    // layout shape as default — [guard][usable] — just tighter usable. Slab
    // size is correspondingly smaller (≈80 MB per slab × few slabs).
    static constexpr size_t kArenaSmallSlotGuard       = 4096;             // 4KB software guard
    static constexpr size_t kArenaSmallSlotUsable      = 16u << 10;        // 16KB usable
    static constexpr size_t kArenaSmallSlotSize        = kArenaSmallSlotGuard + kArenaSmallSlotUsable;
    static constexpr size_t kArenaSmallSlotsPerSlab    = 4096;             // slots per small slab
    static constexpr size_t kArenaSmallSlabSize        = kArenaSmallSlotSize * kArenaSmallSlotsPerSlab;

    struct ArenaSlab {
        void* base = nullptr;
        size_t size = 0;
    };

    std::vector<ArenaSlab> arena_slabs_;        // default-class slabs (for drain)
    std::vector<ArenaSlab> small_arena_slabs_;  // small-class slabs (for drain)
    std::vector<StackRegion> small_free_list_;  // small-class free list
    std::atomic<size_t> small_allocations_{0};
    // free_list_ holds default-class arena StackRegions (with overflow_limit set)
#endif


public:
    // Initial committed region per stack on Windows (at the top, where RSP
    // starts).  The rest of the 1 MB virtual region is MEM_RESERVE, committed
    // on demand by the VEH handler.  Exposed here so spawn() can pass this
    // value to make_fcontext, which stores it as NT_TIB StackLimit.
    static constexpr size_t kInitialCommitSize = 64 * 1024;  // 64 KB
};

} // namespace csp::detail
