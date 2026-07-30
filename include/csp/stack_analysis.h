#pragma once

#include <cstddef>

namespace csp {

struct stack_analysis {
    size_t max_depth;   // Maximum SP displacement in bytes across all paths
    bool is_exact;      // false if unresolvable indirect calls or other unknowns
};

struct stack_analysis_options {
    size_t indirect_call_budget = 2048; // Budget per unresolvable BLR (bytes)
    int max_call_depth = 64;            // Max call-following depth. Clamped to
                                        // [1, 64] (64 is a hard compile-time
                                        // cap); deeper call chains degrade to
                                        // a budgeted, inexact result exactly
                                        // like a detected recursion cycle.
    size_t max_instructions = 100000;   // Safety limit on total instructions analyzed
};

// Analyze the function at `fn` and return its estimated max stack depth.
// If `data` is non-null, it is used to resolve indirect calls: BLR
// targets loaded from data-derived addresses are read from live memory
// and their depth expressions are evaluated recursively.
stack_analysis analyze_stack_depth(
    const void* fn,
    const void* data = nullptr,
    stack_analysis_options opts = {});

// Cache-only lookup: returns a previously computed result if available,
// otherwise returns {stack_size_default, false}. Safe to call from
// threads with limited stack space (no recursive analysis).
stack_analysis analyze_stack_depth_cached(
    const void* fn,
    const void* data = nullptr,
    stack_analysis_options opts = {});

namespace detail {

// 🎯T52.4: the spawn-side async-analysis stub. Fn-keyed result-cache
// lookup only — on a hit returns the published result; on a miss returns
// the conservative {32 KiB, inexact} sentinel and enqueues `fn` on a
// fixed-capacity MPSC ring for the analysis worker (ring-full = the
// request is silently dropped; a later spawn of the same entry retries).
// Allocation-free, never suspends, never analyses: this is the only
// stack-analysis code that executes on imp stacks, and it must stay
// walkable (test/stack_analysis.test.cc asserts is_exact on it).
stack_analysis stack_analysis_lookup_or_request(const void* fn) noexcept;

// 🎯T52.4 worker lifecycle, driven by Runtime::init()/shutdown() under
// CSP_ANALYSE_STACKS. Idempotent; repeated shutdown+re-init cycles
// restart the worker. The worker is a plain OS thread (not an imp): it
// dequeues requests, runs the ordinary synchronous analysis on its own
// full-size stack, and publishes through the spinlocked result caches.
void start_stack_analysis_worker();
void stop_stack_analysis_worker();

} // namespace detail

namespace internal {

// 🎯T52.4 test hook: block until every analysis request enqueued before
// this call has been dequeued and its result published (or the worker is
// not running, in which case it returns immediately). Test-only; call
// from a plain OS thread, not from inside an imp.
void analysis_quiesce();

} // namespace internal

} // namespace csp
