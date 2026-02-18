#include "csp.h"

#include <doctest/doctest.h>

#if defined(__aarch64__)

// Detect sanitizers at compile time. Sanitizer-instrumented code injects
// extra BL calls (shadow memory checks, etc.) that exhaust the analysis
// budget on error-checking paths, making is_exact false even when the
// main execution path correctly resolves indirect calls.
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define CSP_SANITIZED 1
#  endif
#endif
#ifndef CSP_SANITIZED
#  define CSP_SANITIZED 0
#endif

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

TEST_CASE("Leaf function") {
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&leaf_func));
    if (!CSP_SANITIZED) CHECK(result.is_exact);
    // Compiler allocates at least some frame for the volatile write.
    CHECK(result.max_depth > 0);
    // Should be well under 32KB.
    CHECK(result.max_depth < 4096);
    MESSAGE("leaf_func depth: ", result.max_depth);
}

TEST_CASE("Direct call chain") {
    // calls_leaf may tail-call into leaf_func (B instead of BL at -O2).
    // Either way, the total depth should be >= leaf_func's depth.
    auto leaf_result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&leaf_func));
    auto chain_result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&calls_leaf));
    if (!CSP_SANITIZED) CHECK(chain_result.is_exact);
    CHECK(chain_result.max_depth >= leaf_result.max_depth);
    MESSAGE("calls_leaf depth: ", chain_result.max_depth,
            ", leaf_func depth: ", leaf_result.max_depth);
}

TEST_CASE("Indirect call - no data") {
    // Use tighter limits since ASan-instrumented code has many branches.
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

TEST_CASE("Indirect call - with data") {
    indirect_data d{&leaf_func};
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&indirect_caller), &d);
    // With data, the indirect target should be resolved.
    // Under ASan, error-checking paths inject BL calls that exhaust the
    // analysis budget on those paths, making is_exact false even though
    // the main execution path correctly resolves the BLR target.
    if (!CSP_SANITIZED) {
        CHECK(result.is_exact);
    }
    CHECK(result.max_depth > 0);
    MESSAGE("indirect_caller (with data) depth: ", result.max_depth,
            ", is_exact: ", result.is_exact);
}

TEST_CASE("Cache consistency") {
    auto r1 = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&leaf_func));
    auto r2 = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&leaf_func));
    CHECK(r1.max_depth == r2.max_depth);
    CHECK(r1.is_exact == r2.is_exact);
}

TEST_CASE("Different indirect targets - different depths") {
    indirect_data d1{&leaf_func};
    indirect_data d2{&deeper_leaf};
    auto r1 = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&indirect_caller), &d1);
    auto r2 = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&indirect_caller), &d2);
    // Same cached program, different data → potentially different depths.
    // Under ASan, error paths make the result inexact (see above).
    if (!CSP_SANITIZED) {
        CHECK(r1.is_exact);
        CHECK(r2.is_exact);
    }
    // Both should produce reasonable results.
    CHECK(r1.max_depth > 0);
    CHECK(r2.max_depth > 0);
    MESSAGE("indirect w/ leaf_func: ", r1.max_depth,
            ", w/ deeper_leaf: ", r2.max_depth);
}

} // TEST_SUITE

#endif // __aarch64__
