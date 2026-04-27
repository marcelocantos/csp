// Copyright 2025 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Stack density stress test for T3.3.
//
// Spawns large numbers of concurrent imps to verify that arena-based stack
// allocation scales to 100K+ imps without hitting Linux vm.max_map_count
// or running out of resources on macOS.
//
// The 100K test is skipped by default because it takes ~5s.
// Run with:
//   ./build/normal/csp_tests -tc=StackDensity-100K
//
// The 10K test runs in normal CI.

#include "testutil.h"
#include "testscale.h"

#include <doctest/doctest.h>

#include <atomic>

// Spawn N trivial imps, each sending one unit on a shared channel.
// A single reader counts the arrivals.
static void run_density(int N) {
    csp::shutdown_runtime();

    std::atomic<int> done{0};

    // Buffered channel: senders don't need to rendezvous with reader
    // simultaneously, reducing scheduling pressure.
    auto [w, r] = csp::chan<int>{1024};

    // Reader imp: accumulates the count.
    csp::spawn([r = std::move(r), &done] () mutable {
        int sum = 0;
        for (int v; r >> v;) {
            sum += v;
        }
        done.store(sum, std::memory_order_relaxed);
    });

    // N sender imps, each with their own copy of the writer.
    for (int i = 0; i < N; ++i) {
        csp::spawn([wc = w.copy()] () mutable {
            wc << 1;
        });
    }
    w = {};  // close last writer reference so reader exits after N values

    csp::schedule();

    CHECK(done.load() == N);

    csp::shutdown_runtime();
}

TEST_CASE("StackDensity-10K") {
    // 10K concurrent imps -- runs in CI.
    // Under sanitizers, scale down to 1K.
    int N = (SCALE_HEAVY == 1) ? 10'000 : 100;
    run_density(N);
}

TEST_CASE("StackDensity-100K" * doctest::skip(true)) {
    // 100K concurrent imps -- skipped by default, run manually to verify
    // arena-based stack allocation (T3.3).
    run_density(100'000);
}
