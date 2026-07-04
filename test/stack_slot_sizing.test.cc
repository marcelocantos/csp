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
// How this guards the invariant, and why it does NOT run a real over-slot
// frame:
//   The bug is a *selection* decision made in spawn() before the imp runs.
//   The buggy path derives a Small slot from the recorded high-water sample
//   (`profile_needed = hw + hw/2 + headroom + sizeof(Imp) <= 16 KB`),
//   independent of the entry's true frame — so we detect the regression by
//   the slot *class actually allocated* (StackPool::small_allocations()),
//   not by provoking an overflow. A faithful "true peak exceeds the slot"
//   entry would, on the *buggy* build, be placed in a Small slot and then
//   overrun it by kilobytes across only the soft guard — silently corrupting
//   a neighbouring arena imp mid-schedule instead of failing this assertion
//   cleanly. So the entry stays shallow and we assert on the decision. (A
//   `volatile char buf[]` would not give a faithful frame anyway: clang -O2
//   coalesces a non-escaping volatile local down to a few bytes — verified
//   `sub sp, sp, #0x20` for a 24 KB buffer.)
//
// The entries stay analyser-inexact (is_exact == false) because they call
// csp::yield(), an unbounded runtime call the walker cannot bound — so the
// *only* route to a Small slot on the buggy build is the profile path, and
// the fix's is_exact gate is what must keep them in the Default slot.
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

// Two *distinct* type-erased entry functions, one per test case, so the
// per-EntryFn high-water table cannot carry state between cases (doctest may
// run cases in name or random order — a shared key made the exact-equality
// precondition below order-dependent). Each writes a different sentinel so
// linker identical-code-folding cannot merge them back into one EntryFn.
// Each is analyser-inexact because it calls csp::yield(), and shallow,
// because the invariant under test is a spawn-time selection, not a runtime
// overflow (see header).
volatile int g_sink_a = 0;
volatile int g_sink_b = 0;

void probe_selection(void*) {
    csp::yield();
    g_sink_a = 1;
    g_probe_ran.fetch_add(1, std::memory_order_relaxed);
}
void probe_profiling(void*) {
    csp::yield();
    g_sink_b = 2;
    g_probe_ran.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

TEST_SUITE("StackSlotSizing") {

// 🎯T33: an inexact-analyser entry must never be placed in a Small slot on
// the strength of a checkpoint-sampled high-water.
TEST_CASE("profiled-inexact-entry-never-gets-small-slot") {
    if constexpr (!kArena || !kAnalyseWired) {
        MESSAGE("Small-slot spawn path requires arena mode + "
                "-DCSP_ANALYSE_STACKS; skipping on this build.");
        return;
    }

    auto* fn = static_cast<csp::internal::EntryFn>(&probe_selection);

    // Simulate the reported failure precondition: a prior shallow instance of
    // this same EntryFn recorded a small high-water — exactly what the
    // do_switch sampler stores for an instance that took a shallow path (the
    // audit's "instance A"). 4 KB sits inside the buggy selection window
    //   profile_needed = hw + hw/2 + 2 KB headroom + sizeof(Imp) <= 16 KB.
    // The key is unique to this case, so the seed is exact — no cross-case
    // contamination of the monotonic-max table can push it out of the window
    // (which would make the buggy build *pass* and void the differential).
    csp::detail::record_stack_high_water(fn, 4 * 1024);
    REQUIRE(csp::detail::get_stack_high_water(fn) == 4 * 1024);

    auto& pool = StackPool::instance();
    size_t small_before = pool.small_allocations();

    g_probe_ran.store(0, std::memory_order_relaxed);
    csp::internal::spawn(fn, nullptr);
    csp::schedule();
    REQUIRE(g_probe_ran.load(std::memory_order_relaxed) == 1);

    size_t small_delta = pool.small_allocations() - small_before;

    // Under the fix, an inexact entry lands in the Default slot. A non-zero
    // delta means spawn() chose a Small slot from the 4 KB checkpoint sample —
    // the T33 defect. (small_allocations() is a process-global counter; this
    // suite spawns no other imp in the window, and daemon/background imps are
    // themselves type-erased/inexact, so none can take a Small slot to inflate
    // the delta.)
    CHECK_MESSAGE(small_delta == 0,
        "an inexact-analyser entry must stay in the Default slot; a non-zero "
        "delta means spawn() chose a Small (16 KB) slot from a "
        "checkpoint-sampled high-water, violating the T3.4.4 invariant that "
        "Small selection is gated on a sound upper bound (is_exact) "
        "(small_delta=" << small_delta << ")");
}

// The profile high-water must still be *recorded* (the 🎯T3.4.4 residue the
// fix preserves): the sampler is not torn out, it just no longer down-sizes
// the slot class. Assert that a spawn with NO pre-seeding leaves a real
// sampled observation in the table — a non-tautological check that profiling
// still runs (contrast: asserting `>= 4 KB` right after recording 4 KB into a
// monotonic map is vacuous).
TEST_CASE("profile-table-still-records-samples") {
    if constexpr (!kArena || !kAnalyseWired) {
        MESSAGE("Requires arena mode + -DCSP_ANALYSE_STACKS; skipping.");
        return;
    }
    auto* fn = static_cast<csp::internal::EntryFn>(&probe_profiling);

    // Fresh, unique, deliberately unseeded key: whatever lands in the table is
    // the sampler's own observation from the spawn below.
    REQUIRE(csp::detail::get_stack_high_water(fn) == 0);

    g_probe_ran.store(0, std::memory_order_relaxed);
    csp::internal::spawn(fn, nullptr);
    csp::schedule();
    REQUIRE(g_probe_ran.load(std::memory_order_relaxed) == 1);

    // do_switch sampled the imp's real depth at its yield checkpoint and
    // stored it — a strictly positive value the fix must not stop recording.
    CHECK(csp::detail::get_stack_high_water(fn) > 0);
}

} // TEST_SUITE("StackSlotSizing")
