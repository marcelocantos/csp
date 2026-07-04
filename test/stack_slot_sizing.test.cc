// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Regression test for 🎯T33 (audit finding F1, docs/audit/fable-2026-07.md).
//
// The defect: spawn() selected StackClass::Small purely from the
// checkpoint-sampled per-entry high-water table, with no is_exact gate and
// no sound upper bound. The high-water is sampled only at do_switch
// checkpoints, so a helper that grows and unwinds *between* two channel ops
// is never observed; worse, the table is keyed per EntryFn and shared across
// all instances, so a shallow instance's small sample gates a deep
// instance's Small allocation. Arena Small slots have only a 4 KB software
// guard (no PROT_NONE page on arm64), so an imp that outgrows a
// wrongly-chosen Small slot silently corrupts an adjacent imp's control
// block and stack.
//
// The invariant (fable-2026-07.md:29, T3.4.4 acceptance in bullseye.yaml):
// a Small slot must never be chosen from an empirical high-water estimate.
// Small selection must be gated strictly on a sound static upper bound
// (analyser is_exact); the profile only refines the inexact fallback
// magnitude for the never-shrinking Default slot.
//
// This test is only meaningful in arena mode with the analyser wiring
// compiled in (arm64 + -DCSP_ANALYSE_STACKS). On any other build the Small
// slot class does not exist and the whole spawn-side path is #ifdef'd out,
// so the test degrades to a documented skip.

#include "testutil.h"

#include <csp/internal/csp_internal.h>
#include <csp/internal/stack_pool.h>

#include <doctest/doctest.h>

#include <atomic>

namespace {

using csp::detail::StackPool;

constexpr bool kArena =
#if CSP_USE_ARENA_STACKS
    true;
#else
    false;
#endif

constexpr bool kAnalyseWired =
#ifdef CSP_ANALYSE_STACKS
    true;
#else
    false;
#endif

std::atomic<int> g_probe_ran{0};

// A spawned entry function whose *true* peak stack depth is data-dependent
// and occurs strictly between two channel-op checkpoints — exactly the shape
// the checkpoint sampler cannot observe. The volatile buffer is large enough
// (24 KB) to overflow a 16 KB-usable Small slot, but it is allocated and
// consumed after the first yield() and released before the second, so no
// do_switch checkpoint ever samples SP while it is live. The analyser cannot
// resolve this type-erased entry (is_exact == false), so the *only* route to
// a Small slot is the buggy profile path.
void deep_between_checkpoints(void*) {
    csp::yield();  // checkpoint 1 — shallow
    {
        volatile char buf[24 * 1024];
        for (size_t i = 0; i < sizeof(buf); i += 4096) buf[i] = static_cast<char>(i);
        // Touch the deepest byte so the compiler cannot elide the frame.
        buf[sizeof(buf) - 1] = 7;
    }
    csp::yield();  // checkpoint 2 — shallow again; the 24 KB peak is gone
    g_probe_ran.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

TEST_SUITE("StackSlotSizing") {

// 🎯T33: an entry function whose true peak can exceed the Small slot must
// never be placed in one on the strength of a checkpoint-sampled high-water.
TEST_CASE("profiled-inexact-entry-never-gets-small-slot") {
    if constexpr (!kArena || !kAnalyseWired) {
        MESSAGE("Small-slot spawn path requires arena mode + "
                "-DCSP_ANALYSE_STACKS; skipping on this build.");
        return;
    }

    auto* fn = static_cast<csp::internal::EntryFn>(&deep_between_checkpoints);

    // Simulate the reported failure precondition: a prior shallow instance of
    // this same EntryFn recorded a small high-water. This is exactly what the
    // do_switch sampler would store for an instance that took a shallow path
    // (the audit's "instance A"). The value is chosen to sit safely inside
    // the buggy selection window:
    //   profile_needed = hw + hw/2 + 2 KB headroom + sizeof(Imp) <= 16 KB
    // so hw = 4 KB yields ~8 KB + sizeof(Imp) < 16 KB regardless of the exact
    // Imp size.
    csp::detail::record_stack_high_water(fn, 4 * 1024);
    REQUIRE(csp::detail::get_stack_high_water(fn) == 4 * 1024);

    auto& pool = StackPool::instance();
    size_t small_before = pool.small_allocations();

    g_probe_ran.store(0, std::memory_order_relaxed);
    csp::internal::spawn(fn, nullptr);
    csp::schedule();
    REQUIRE(g_probe_ran.load(std::memory_order_relaxed) == 1);

    size_t small_delta = pool.small_allocations() - small_before;

    // The entry's analyser result is inexact (type-erased spawn entry), and
    // its *true* peak (24 KB) exceeds the 16 KB Small usable region. Selecting
    // Small here on the strength of the 4 KB checkpoint sample is the defect:
    // the imp can descend past its slot into a neighbour with only a soft
    // guard between them. A correct sizing path leaves it in the Default slot.
    CHECK_MESSAGE(small_delta == 0,
        "a profiled-but-inexact entry (true peak 24 KB) must stay in the "
        "Default slot; a non-zero delta means spawn() chose a Small (16 KB) "
        "slot from a checkpoint-sampled high-water, violating the T3.4.4 "
        "invariant that Small selection is gated on a sound upper bound "
        "(small_delta=" << small_delta << ")");
}

// The profile high-water must still refine the analyser's indirect_call_budget
// for the *Default*-slot path (the 🎯T3.4.4 residue the fix must preserve): the
// recorded observation is not discarded, it just no longer down-sizes the slot
// class. We assert the recorded value survives a spawn (recording is
// unconditional and the fix must not tear out the table).
TEST_CASE("profile-table-survives-spawn-for-budget-refinement") {
    if constexpr (!kArena || !kAnalyseWired) {
        MESSAGE("Requires arena mode + -DCSP_ANALYSE_STACKS; skipping.");
        return;
    }
    auto* fn = static_cast<csp::internal::EntryFn>(&deep_between_checkpoints);
    csp::detail::record_stack_high_water(fn, 4 * 1024);

    g_probe_ran.store(0, std::memory_order_relaxed);
    csp::internal::spawn(fn, nullptr);
    csp::schedule();
    REQUIRE(g_probe_ran.load(std::memory_order_relaxed) == 1);

    // The table still holds an observation for fn (the fix keeps profiling;
    // it only stops using the sample to pick Small).
    CHECK(csp::detail::get_stack_high_water(fn) >= 4 * 1024);
}

} // TEST_SUITE("StackSlotSizing")
