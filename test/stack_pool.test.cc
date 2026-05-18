// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the dual-class StackPool (🎯T3.4.1).
//
// The Small slot class is the runtime side of the analyser-driven
// allocation path. The wiring in spawn() is gated by CSP_ANALYSE_STACKS,
// but the pool's Small-class API is unconditional so it can be tested
// directly here without recompiling the world.

#include "testutil.h"

#include <csp/internal/stack_pool.h>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>

using csp::detail::StackClass;
using csp::detail::StackPool;
using csp::detail::StackRegion;

namespace {

constexpr bool kArena =
#if CSP_USE_ARENA_STACKS
    true;
#else
    false;
#endif

} // namespace

TEST_SUITE("StackPool") {

TEST_CASE("default-allocate-release-roundtrip") {
    auto& pool = StackPool::instance();
    auto region = pool.allocate(StackClass::Default);
    CHECK(region.base != nullptr);
    CHECK(region.total_size > 0);
    CHECK(region.cls == StackClass::Default);
    if (kArena) {
        CHECK(region.overflow_limit != nullptr);
    }
    pool.release(region);
}

TEST_CASE("small-allocate-shape") {
    if constexpr (!kArena) {
        MESSAGE("Small slot class is arena-only; skipping on this build.");
        return;
    }
    auto& pool = StackPool::instance();
    auto region = pool.allocate(StackClass::Small);
    CHECK(region.base != nullptr);
    CHECK(region.cls == StackClass::Small);
    // Tight slot: must be smaller than the default 128 KB slot and at least
    // as large as the advertised usable budget plus its guard.
    CHECK(region.total_size <= 64 * 1024);
    CHECK(region.total_size >= StackPool::small_slot_usable_bytes());
    // Guard zone exists for arena slots.
    CHECK(region.overflow_limit != nullptr);
    auto guard_size = static_cast<size_t>(
        static_cast<const char*>(region.overflow_limit) -
        static_cast<const char*>(region.base));
    CHECK(guard_size >= 4096);
    pool.release(region);
}

TEST_CASE("small-pool-recycles") {
    if constexpr (!kArena) return;
    auto& pool = StackPool::instance();
    // Drain the pool first so the next allocation comes from a fresh slab.
    pool.drain();

    auto first = pool.allocate(StackClass::Small);
    auto saved_base = first.base;
    pool.release(first);

    // Releasing pushes to small_free_list_; the next small allocate should
    // serve from there (LIFO), returning the same region.
    auto second = pool.allocate(StackClass::Small);
    CHECK(second.base == saved_base);
    CHECK(second.cls == StackClass::Small);
    pool.release(second);
}

TEST_CASE("small-allocations-counter") {
    if constexpr (!kArena) return;
    auto& pool = StackPool::instance();
    size_t before = pool.small_allocations();
    auto a = pool.allocate(StackClass::Small);
    auto b = pool.allocate(StackClass::Small);
    size_t after = pool.small_allocations();
    CHECK(after == before + 2);
    pool.release(a);
    pool.release(b);
}

TEST_CASE("small-and-default-distinct-pools") {
    if constexpr (!kArena) return;
    auto& pool = StackPool::instance();
    auto def = pool.allocate(StackClass::Default);
    auto sm = pool.allocate(StackClass::Small);
    CHECK(def.total_size > sm.total_size);
    CHECK(def.cls == StackClass::Default);
    CHECK(sm.cls == StackClass::Small);
    pool.release(sm);
    pool.release(def);
}

TEST_CASE("small-slot-usable-bytes-is-positive-on-arena") {
    if constexpr (kArena) {
        CHECK(StackPool::small_slot_usable_bytes() > 0);
        // Must be at least enough for an Imp plus a non-trivial stack
        // budget — choose 8 KB as a sanity floor below the documented 16 KB.
        CHECK(StackPool::small_slot_usable_bytes() >= 8 * 1024);
    } else {
        // On non-arena builds the gate value is 0, which makes the spawn()
        // analyser check `needed <= small_slot_usable_bytes()` always fail.
        CHECK(StackPool::small_slot_usable_bytes() == 0);
    }
}

} // TEST_SUITE("StackPool")
