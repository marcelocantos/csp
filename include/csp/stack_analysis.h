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

} // namespace csp
