// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Tests for the per-entry-function stack high-water table (🎯T3.4.4).
//
// The table is populated at every CSP suspend checkpoint and read at
// spawn time. Tests here exercise the record/get API directly plus the
// end-to-end spawn-then-observe flow.

#include "testutil.h"

#include <csp/internal/csp_internal.h>

#include <doctest/doctest.h>

#include <atomic>

namespace {

// A test-only sentinel that lets us match entry functions across runs.
// We never spawn() this directly — it's used as a synthetic key for the
// record/get API tests.
void sentinel_entry(void*) {}

// A spawned function whose body forces a non-trivial suspend depth so the
// recording is observably non-zero.
std::atomic<int> g_ran{0};

void spawned_entry(void*) {
    volatile char buf[256];
    for (int i = 0; i < 256; ++i) buf[i] = static_cast<char>(i);
    // Yield — this hits do_switch(), where the high-water is recorded.
    csp::yield();
    g_ran.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

TEST_SUITE("StackProfile") {

TEST_CASE("record-and-get-roundtrip") {
    // Use a unique sentinel address so the test is order-independent.
    auto* fn = reinterpret_cast<csp::internal::EntryFn>(&sentinel_entry);
    // We can't reliably assert the starting value (the test binary may
    // have spawned other things on this fn pointer in some other run —
    // unlikely for a static sentinel, but we don't depend on it).
    size_t before = csp::detail::get_stack_high_water(fn);

    csp::detail::record_stack_high_water(fn, before + 100);
    CHECK(csp::detail::get_stack_high_water(fn) == before + 100);

    // Recording a lower value does not lower the high-water.
    csp::detail::record_stack_high_water(fn, before + 50);
    CHECK(csp::detail::get_stack_high_water(fn) == before + 100);

    // Recording a higher value raises it.
    csp::detail::record_stack_high_water(fn, before + 500);
    CHECK(csp::detail::get_stack_high_water(fn) == before + 500);
}

TEST_CASE("null-entry-function-is-noop") {
    csp::detail::record_stack_high_water(nullptr, 1234);
    CHECK(csp::detail::get_stack_high_water(nullptr) == 0);
}

TEST_CASE("spawn-records-high-water-via-suspend") {
#ifndef CSP_ANALYSE_STACKS
    // Suspend-checkpoint recording is compiled out with the analyser —
    // its only consumer is spawn()'s CSP_ANALYSE_STACKS slot-sizing
    // path, and the record cost (mutex + hash probe per suspend) is
    // pure hot-path waste otherwise (🎯T35). Same gating convention as
    // StackSlotSizing.
    MESSAGE("Recording at suspend checkpoints requires -DCSP_ANALYSE_STACKS; "
            "skipping on this build.");
#else
    // The first time we run spawned_entry in this process, its high-water
    // should be zero (no prior observation).
    auto* fn = static_cast<csp::internal::EntryFn>(&spawned_entry);
    size_t before = csp::detail::get_stack_high_water(fn);

    g_ran.store(0, std::memory_order_relaxed);
    csp::internal::spawn(fn, nullptr);
    csp::schedule();

    CHECK(g_ran.load(std::memory_order_relaxed) == 1);

    size_t after = csp::detail::get_stack_high_water(fn);
    // After running, the table must hold a non-zero observation for fn.
    CHECK(after > before);
    // Sanity: the value should be at least the size of the local buffer
    // we forced onto the stack.
    CHECK(after >= 256);
    // And bounded by the slot size (we're not corrupting memory).
    CHECK(after < 200 * 1024);
    MESSAGE("spawned_entry recorded high-water: ", after, " bytes");
#endif
}

} // TEST_SUITE("StackProfile")
