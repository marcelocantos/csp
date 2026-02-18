#pragma once

#include <cstddef>
#include <mutex>
#include <vector>

// Sanitizer detection: under ASan/TSan, shadow memory scales with mapped VA,
// so we fall back to heap allocation (new[]/delete[]).
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    (defined(__has_feature) && (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer)))
#define CSP_USE_MMAP_STACKS 0
#else
#define CSP_USE_MMAP_STACKS 1
#endif

namespace csp::detail {

struct StackRegion {
    void* base = nullptr;
    size_t total_size = 0;
    explicit operator bool() const { return base != nullptr; }
};

class StackPool {
public:
    static StackPool& instance();

    // Allocate a stack region. Returns a pooled or fresh mmap'd region.
    StackRegion allocate();

    // Return a stack to the pool. Reclaims physical pages via madvise.
    void release(StackRegion region);

    // Reclaim unused stack pages below the current SP.
    void maybe_shrink(StackRegion const& region, void* current_sp);

    // Unmap all pooled stacks. Called during shutdown.
    void drain();

    size_t page_size() const { return page_size_; }

private:
    StackPool();

    StackRegion mmap_new();
    void munmap_region(StackRegion region);

    size_t page_size_;
    size_t stack_size_;     // total mmap size (guard + usable)

    std::mutex mu_;
    std::vector<StackRegion> free_list_;

    static constexpr size_t kDefaultStackSize = 1 << 20;  // 1MB
    static constexpr size_t kMaxPooled = 256;
};

} // namespace csp::detail
