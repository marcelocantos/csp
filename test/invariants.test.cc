#include "csp.h"
#include "testscale.h"

#include <doctest/doctest.h>

#include <atomic>
#include <vector>

// B1. Channel cleanup consistency
// Spawns many channel pairs and verifies all channels are cleaned up
// after completion. Catches leaked channels from queue corruption or
// double-enqueue (which could skip cleanup).
TEST_CASE("Invariant - ChannelCleanup") {
    csp::init_runtime(4);

    constexpr int N = 1000 / SCALE_MEDIUM;
    std::atomic<int> completed{0};

    for (int i = 0; i < N; ++i) {
        auto [w, r] = csp::chan<int>{};
        csp::spawn([w = std::move(w), i] { w << i; });
        csp::spawn([r = std::move(r), &completed] {
            int v;
            if (r >> v) completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    csp::schedule();

    CHECK_EQ(N, completed.load());
    // All channels must be cleaned up: refcounts dropped, no leaks.
    CHECK_EQ(0, csp::internal::channel_count(0));
    CHECK_EQ(0, csp::internal::channel_count(1));

    csp::shutdown_runtime();
}

// B2. No double-enqueue (exact count)
// Rapid channel ops across multiple Ps. Any double-enqueue would cause
// an MT to run twice (inflating count) or be skipped (deflating it).
TEST_CASE("Invariant - ExactMessageCount") {
    csp::init_runtime(4);

    constexpr int PAIRS = 500 / SCALE_MEDIUM;
    constexpr int MSGS = 10;
    std::atomic<int64_t> total{0};

    for (int p = 0; p < PAIRS; ++p) {
        auto [w, r] = csp::chan<int>{};
        csp::spawn([w = std::move(w)] {
            for (int i = 0; i < MSGS; ++i) {
                w << (i + 1);
            }
        });
        csp::spawn([r = std::move(r), &total] {
            for (int v; r >> v;) {
                total.fetch_add(v, std::memory_order_relaxed);
            }
        });
    }

    csp::schedule();

    // sum(1..MSGS) = MSGS*(MSGS+1)/2, repeated PAIRS times.
    int64_t expected = (int64_t)PAIRS * MSGS * (MSGS + 1) / 2;
    CHECK_EQ(expected, total.load());

    csp::shutdown_runtime();
}

// B2 variant: single-P mode (no M:N).
TEST_CASE("Invariant - ExactMessageCountSingleP") {
    constexpr int PAIRS = 200 / SCALE_MEDIUM;
    constexpr int MSGS = 10;
    std::atomic<int64_t> total{0};

    for (int p = 0; p < PAIRS; ++p) {
        auto [w, r] = csp::chan<int>{};
        csp::spawn([w = std::move(w)] {
            for (int i = 0; i < MSGS; ++i) {
                w << (i + 1);
            }
        });
        csp::spawn([r = std::move(r), &total] {
            for (int v; r >> v;) {
                total.fetch_add(v, std::memory_order_relaxed);
            }
        });
    }

    csp::schedule();

    int64_t expected = (int64_t)PAIRS * MSGS * (MSGS + 1) / 2;
    CHECK_EQ(expected, total.load());
}

// B3. Suspending window stress
// Maximize time in the suspending window: many MTs doing alt() with
// immediate-ready peers (fast path through suspending_=true → unlock →
// wake_pending check → continue).
TEST_CASE("Invariant - SuspendingWindowStress") {
    csp::init_runtime(4);

    constexpr int N = 10000 / SCALE_HEAVY;
    std::atomic<int> completed{0};

    for (int i = 0; i < N; ++i) {
        auto [w, r] = csp::chan<int>{};
        // Writer sends immediately — reader will either find it ready
        // in phase 1 (no suspend) or will suspend and get woken by
        // drain_suspended. Both paths must work.
        csp::spawn([w = std::move(w), i] { w << i; });
        csp::spawn([r = std::move(r), &completed] {
            int v = 0;
            int which = csp::prialt(r >> v);
            if (which >= 0) {
                completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    csp::schedule();

    CHECK_EQ(N, completed.load());

    csp::shutdown_runtime();
}

// B3 variant: alt with multiple channels, one ready, one dead.
// Uses data chanop (r >> v) on the dead channel, not vulture (~r).
// The fix (1d6a732) defers dead-data-chanop results until after
// scanning for ready peers on other channels. So the ready peer
// should always win regardless of alt scan order.
//
// Must run in single-P mode: cooperative scheduling guarantees the
// writer blocks on the channel before the reader runs alt(), ensuring
// a ready peer is always available. In M:N mode, the writer may not
// have run yet, leaving no ready peer and making the dead channel the
// only option (which is correct behavior, but not what we're testing).
TEST_CASE("Invariant - AltReadyVsDeadStress") {
    constexpr int N = 5000 / SCALE_HEAVY;
    std::atomic<int> data_results{0};
    std::atomic<int> dead_results{0};

    for (int i = 0; i < N; ++i) {
        auto [w1, r1] = csp::chan<int>{};
        csp::chan<int> dead_ch;
        // Close the dead channel's writer immediately.
        dead_ch.w = {};

        // Writer sends to r1.
        csp::spawn([w1 = std::move(w1), i] { w1 << i; });

        // Reader does alt on both channels: one has a ready peer,
        // one is dead. Both are data chanops (r >> v), so the dead
        // channel result is deferred until after scanning all channels.
        csp::spawn([r1 = std::move(r1), dr = std::move(dead_ch.r),
                     &data_results, &dead_results] {
            int v1 = 0, v2 = 0;
            int which = csp::alt(r1 >> v1, dr >> v2);
            if (which == 0) {
                data_results.fetch_add(1, std::memory_order_relaxed);
            } else {
                dead_results.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    csp::schedule();

    // Every iteration must produce exactly one result.
    CHECK_EQ(N, data_results.load() + dead_results.load());
    // In single-P mode, the writer always blocks before the reader runs
    // alt(), so the ready peer always wins over the dead channel.
    CHECK_EQ(N, data_results.load());
}
