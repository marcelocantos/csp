#include "csp.h"

#include <doctest/doctest.h>

#if defined(__aarch64__)

// Detect sanitizers at compile time.
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define CSP_SANITIZED 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  undef CSP_SANITIZED
#  define CSP_SANITIZED 1
#endif
#ifndef CSP_SANITIZED
#  define CSP_SANITIZED 0
#endif

// Under sanitizers, instrumented code contains BL calls into ASan/TSan
// runtime (shadow memory checks, etc.) that can lead the instruction
// walker into unmapped memory.  Skip the entire suite — the analyzer
// is already bypassed at runtime (see spawn()).
#if !CSP_SANITIZED

// Prevent inlining so each function gets its own stack frame.
// At -O2, the compiler allocates minimal frames, so we check relative
// relationships rather than absolute buffer sizes.

__attribute__((noinline)) static void leaf_func(void*) {
    volatile char buf[64];
    buf[0] = 1;
}

__attribute__((noinline)) static void deeper_leaf(void*) {
    volatile char buf[64];
    // Touch multiple elements to encourage larger frame allocation.
    for (int i = 0; i < 64; ++i)
        buf[i] = static_cast<char>(i);
}

__attribute__((noinline)) static void calls_leaf(void*) {
    volatile char buf[64];
    buf[0] = 1;
    leaf_func(nullptr);
}

// A function that calls through a function pointer stored in its data arg.
struct indirect_data {
    void (*fn)(void*);
};

__attribute__((noinline)) static void indirect_caller(void* data) {
    volatile char buf[64];
    buf[0] = 1;
    auto* d = static_cast<indirect_data*>(data);
    d->fn(nullptr);
}

TEST_SUITE("StackAnalysis") {

TEST_CASE("Leaf-function") {
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&leaf_func));
    CHECK(result.is_exact);
    // Compiler allocates at least some frame for the volatile write.
    CHECK(result.max_depth > 0);
    // Should be well under 4KB for a trivial leaf.
    CHECK(result.max_depth < 4096);
    MESSAGE("leaf_func depth: ", result.max_depth);
}

TEST_CASE("Direct-call-chain") {
    // calls_leaf may tail-call into leaf_func (B instead of BL at -O2).
    // Either way, the total depth should be >= leaf_func's depth.
    auto leaf_result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&leaf_func));
    auto chain_result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&calls_leaf));
    CHECK(chain_result.is_exact);
    CHECK(chain_result.max_depth >= leaf_result.max_depth);
    MESSAGE("calls_leaf depth: ", chain_result.max_depth,
            ", leaf_func depth: ", leaf_result.max_depth);
}

TEST_CASE("Indirect-call---no-data") {
    csp::stack_analysis_options opts;
    opts.max_instructions = 10000;
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&indirect_caller), nullptr, opts);
    // Without data, can't resolve the indirect call.
    CHECK_FALSE(result.is_exact);
    CHECK(result.max_depth > 0);
    MESSAGE("indirect_caller (no data) depth: ", result.max_depth,
            ", is_exact: ", result.is_exact);
}

TEST_CASE("Indirect-call---with-data") {
    indirect_data d{&leaf_func};
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&indirect_caller), &d);
    // With data, the indirect target should be resolved.
    CHECK(result.is_exact);
    CHECK(result.max_depth > 0);
    MESSAGE("indirect_caller (with data) depth: ", result.max_depth,
            ", is_exact: ", result.is_exact);
}

TEST_CASE("Cache-consistency") {
    auto r1 = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&leaf_func));
    auto r2 = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&leaf_func));
    CHECK(r1.max_depth == r2.max_depth);
    CHECK(r1.is_exact == r2.is_exact);
}

TEST_CASE("Different-indirect-targets---different-depths") {
    indirect_data d1{&leaf_func};
    indirect_data d2{&deeper_leaf};
    auto r1 = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&indirect_caller), &d1);
    auto r2 = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&indirect_caller), &d2);
    // Same cached program, different data → potentially different depths.
    CHECK(r1.is_exact);
    CHECK(r2.is_exact);
    // Both should produce reasonable results.
    CHECK(r1.max_depth > 0);
    CHECK(r2.max_depth > 0);
    MESSAGE("indirect w/ leaf_func: ", r1.max_depth,
            ", w/ deeper_leaf: ", r2.max_depth);
}

// ---- New tests for T3.4 improvements ----

// A virtual-dispatch-style scenario: object pointer + vtable lookup.
// The struct stores a function pointer at a known offset (simulating a vtable).
struct vtable_obj {
    void (*method)(void*);  // offset 0 — simulates first vtable slot
};

__attribute__((noinline)) static void vtable_caller(void* data) {
    volatile char buf[64];
    buf[0] = 1;
    auto* obj = static_cast<vtable_obj*>(data);
    obj->method(nullptr);  // BLR via function pointer from struct
}

TEST_CASE("Vtable-like-indirect-call---no-data") {
    // Without data the indirect call falls back to budget.
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&vtable_caller), nullptr);
    CHECK_FALSE(result.is_exact);
}

TEST_CASE("Vtable-like-indirect-call---with-data") {
    vtable_obj obj{&leaf_func};
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&vtable_caller), &obj);
    CHECK(result.is_exact);
    CHECK(result.max_depth > 0);
    MESSAGE("vtable_caller (with data) depth: ", result.max_depth,
            ", is_exact: ", result.is_exact);
}

// Interprocedural data forwarding: A calls B, passing a sub-pointer from
// the closure. B calls through a function pointer inside the sub-object.
struct inner_data {
    void (*callback)(void*);
};

struct outer_data {
    int tag;
    inner_data* inner;  // sub-pointer at offset 8
};

__attribute__((noinline, optnone))
static void inner_caller(void* data) {
    volatile char buf[128];
    buf[0] = 1;
    auto* d = static_cast<inner_data*>(data);
    d->callback(nullptr);
}

__attribute__((noinline))
static void outer_caller(void* data) {
    volatile char buf[64];
    buf[0] = 1;
    auto* d = static_cast<outer_data*>(data);
    inner_caller(d->inner);  // passes sub-pointer — interprocedural forwarding
}

TEST_CASE("Interprocedural-data-forwarding---no-data") {
    // Without data, both levels fall back to budget.
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&outer_caller), nullptr);
    CHECK_FALSE(result.is_exact);
}

TEST_CASE("Interprocedural-data-forwarding---with-data") {
    inner_data inner{&leaf_func};
    outer_data outer{0, &inner};
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&outer_caller), &outer);
    // With forwarding, the inner indirect call should be resolved.
    // At minimum the result should be non-zero and not crash.
    CHECK(result.max_depth > 0);
    MESSAGE("outer_caller depth: ", result.max_depth,
            ", is_exact: ", result.is_exact);
}

// Branch pruning via CBZ/CBNZ: a function with two paths of very different
// stack depth, dispatched on a boolean captured in the closure.
struct tag_data {
    int which;  // 0 = small path, 1 = large path
};

__attribute__((noinline))
static void small_path(void*) {
    volatile char buf[64];
    buf[0] = 1;
}

__attribute__((noinline))
static void large_path(void*) {
    volatile char buf[512];
    for (int i = 0; i < 512; ++i) buf[i] = static_cast<char>(i);
}

// This function is intentionally written to use a conditional branch
// that the analyzer may or may not be able to prune. We don't assert
// pruning (it requires the compiler to use CBZ/CBNZ on the loaded tag),
// but we do assert correctness in both data cases.
__attribute__((noinline))
static void branching_caller(void* data) {
    volatile char buf[32];
    buf[0] = 1;
    auto* d = static_cast<tag_data*>(data);
    if (d->which == 0) {
        small_path(nullptr);
    } else {
        large_path(nullptr);
    }
}

TEST_CASE("Branch-with-data---small-path") {
    tag_data d{0};
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&branching_caller), &d);
    // With data, the CBZ on d->which is folded at walk time: only the
    // small_path branch is explored, giving an exact result.
    CHECK(result.is_exact);
    CHECK(result.max_depth > 0);
    // Small path is well under 512 bytes (it only has a tiny local buffer).
    CHECK(result.max_depth < 512);
    MESSAGE("branching_caller (small) depth: ", result.max_depth,
            ", is_exact: ", result.is_exact);
}

TEST_CASE("Branch-with-data---large-path") {
    tag_data d{1};
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&branching_caller), &d);
    // The large path has a loop (stack canary check) that may exceed the
    // instruction budget, so is_exact may be false — but the result should
    // be at least as large as the small path's exact result.
    CHECK(result.max_depth > 0);
    tag_data d_small{0};
    auto small_result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&branching_caller), &d_small);
    CHECK(result.max_depth >= small_result.max_depth);
    MESSAGE("branching_caller (large) depth: ", result.max_depth,
            ", is_exact: ", result.is_exact);
}

} // TEST_SUITE

#endif // !CSP_SANITIZED
#endif // __aarch64__
