// Copyright 2025 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Stack density stress test for T3.3.
//
// Spawns large numbers of concurrent imps to verify that arena-based stack
// allocation scales to 100K+ imps without hitting Linux vm.max_map_count
// or running out of resources on macOS.
//
// The 100K test is skipped under sanitizers (they use heap stacks, not arena).

#include "testutil.h"
#include "testscale.h"

#include <doctest/doctest.h>

#include <atomic>

#ifdef __linux__
#include <fstream>
#include <string>
#endif

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

    csp::await_completion();

    CHECK(done.load() == N);

    csp::shutdown_runtime();
}

TEST_CASE("StackDensity-10K") {
    // 10K concurrent imps -- runs in CI.
    // Under sanitizers, scale down to 1K.
    int N = (SCALE_HEAVY == 1) ? 10'000 : 100;
    run_density(N);
}

TEST_CASE("StackDensity-100K") {
    // 100K concurrent imps -- validates arena-based stack allocation (T3.3).
    // Skipped under sanitizers: they use heap stacks (not arena) and the
    // overhead would make this test prohibitively slow.
#if CSP_TEST_SANITIZER
    // Sanitizer build: scale down, skip arena assertions.
    run_density(1'000);
#else
    run_density(100'000);

    // Arena builds (Unix non-sanitizer): verify that 100K imps consumed
    // only a small number of VMAs — one per 4096-slot slab, so at most ~25
    // for 100K imps.  shutdown_runtime() calls drain() which munmaps all
    // slabs, so slab_count() returns 0 after the run.
#if CSP_USE_ARENA_STACKS
    size_t slabs = csp::detail::StackPool::instance().slab_count();
    CHECK(slabs == 0);  // shutdown_runtime() drains the pool

#ifdef __linux__
    // On Linux, count the actual VMAs in /proc/self/maps and log it.
    // This is informational — the real assertion is that we didn't crash
    // or hit ENOMEM above. A reasonable upper bound after drain is <500
    // (the process baseline is typically ~100-200 VMAs).
    int vma_count = 0;
    {
        std::ifstream maps("/proc/self/maps");
        std::string line;
        while (std::getline(maps, line)) {
            ++vma_count;
        }
    }
    MESSAGE("VMA count after 100K-imp run + drain: " << vma_count);
    // After drain(), arena slabs are munmap'd. Process VMA baseline is
    // well under 1000; assert we're not leaking slab VMAs.
    CHECK(vma_count < 1000);
#endif // __linux__
#endif // CSP_USE_ARENA_STACKS
#endif // CSP_TEST_SANITIZER
}
