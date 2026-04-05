#include "csp.h"
#include "testscale.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// A1. steal_work contention
// ---------------------------------------------------------------------------
// Saturate one P's local queue with many compute-bound MTs that yield
// frequently, forcing the other P to steal. Verify total count matches
// (no MT lost or duplicated).

TEST_CASE("Protocol---A1-steal_work-contention") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    constexpr int N = 200 / SCALE_LIGHT;
    std::atomic<int> count{0};

    // Spawn all MTs quickly so they land on P0's local queue.
    // Each MT yields several times to give the stealing P opportunities.
    for (int i = 0; i < N; ++i) {
        csp::spawn([&] {
            for (int y = 0; y < 5; ++y) {
                csp::yield();
            }
            count.fetch_add(1, std::memory_order_relaxed);
        });
    }

    csp::schedule();

    CHECK(N == count.load());

    csp::shutdown_runtime();
}

// ---------------------------------------------------------------------------
// A2. Concurrent channel close + alt sleep
// ---------------------------------------------------------------------------
// N MTs block in alt() on a channel. Another MT closes the channel by
// dropping the writer. All N MTs must wake with a complemented (dead)
// result. No MT hangs.

TEST_CASE("Protocol---A2-concurrent-channel-close-+-alt-sleep") {
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int N = 8;
    auto [w, r] = csp::chan<int>{};
    std::atomic<int> dead_count{0};
    std::atomic<int> data_count{0};

    // Spawn N readers that block in alt() on the channel.
    for (int i = 0; i < N; ++i) {
        csp::spawn([&dead_count, &data_count, r = r.copy()] {
            int val = 0;
            int result = csp::alt(r >> val);
            if (result < 0) {
                dead_count.fetch_add(1, std::memory_order_relaxed);
            } else {
                data_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Give readers time to register as waiters before closing.
    csp::spawn([w = std::move(w)] {
        csp::yield();
        csp::yield();
        // Writer destroyed on scope exit, closing the channel.
    });

    // Release our copies so only the spawned MTs hold endpoints.
    r = {};

    csp::schedule();

    // All N readers should have been woken by the dead channel.
    CHECK(N == dead_count.load());
    CHECK(0 == data_count.load());

    csp::shutdown_runtime();
}

// ---------------------------------------------------------------------------
// A3. Concurrent endpoint release
// ---------------------------------------------------------------------------
// Create a channel, copy writer to N MTs and reader to M MTs. All
// endpoints close simultaneously. Verify no leak (channel count returns
// to baseline) and no double-free (ASan would catch this).

TEST_CASE("Protocol---A3-concurrent-endpoint-release") {
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int N_WRITERS = 10;
    constexpr int N_READERS = 10;

    int baseline_w = csp::internal::channel_count(0);
    int baseline_r = csp::internal::channel_count(1);

    {
        csp::chan<int> ch;

        for (int i = 0; i < N_WRITERS; ++i) {
            csp::spawn([w = ch.w.copy()] {
                // Just hold the endpoint briefly, then drop it.
                csp::yield();
            });
        }

        for (int i = 0; i < N_READERS; ++i) {
            csp::spawn([r = ch.r.copy()] {
                csp::yield();
            });
        }

        ch.release();
    }

    csp::schedule();

    // Channel count should return to baseline (no leaks).
    CHECK(baseline_w == csp::internal::channel_count(0));
    CHECK(baseline_r == csp::internal::channel_count(1));

    csp::shutdown_runtime();
}

// ---------------------------------------------------------------------------
// A4. Early wake path
// ---------------------------------------------------------------------------
// A writer sends immediately so the reader's wake_pending_ is set before
// it context-switches. The reader does alt() and should hit the early-wake
// path in run(Status::detach). Verify the reader gets the value.

TEST_CASE("Protocol---A4-early-wake-path") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    constexpr int ROUNDS = 100 / SCALE_LIGHT;
    std::atomic<int> success{0};

    for (int i = 0; i < ROUNDS; ++i) {
        auto [w, r] = csp::chan<int>{};

        // Writer sends immediately — may complete before reader suspends.
        csp::spawn([w = std::move(w)] {
            w << 42;
        });

        csp::spawn([&success, r = std::move(r)] {
            int val = 0;
            if (r >> val) {
                if (val == 42) {
                    success.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    csp::schedule();

    CHECK(ROUNDS == success.load());

    csp::shutdown_runtime();
}

// ---------------------------------------------------------------------------
// A5. Timer heap boundary (65 timers)
// ---------------------------------------------------------------------------
// fire_timers uses a fixed 64-element expired[] array. Spawn 65 MTs
// that all sleep_until the same near-future deadline. The 65th must be
// deferred to the next fire_timers iteration. Verify all 65 complete.

TEST_CASE("Protocol---A5-timer-heap-boundary") {
    using namespace std::chrono_literals;

    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    constexpr int N = 65;
    std::atomic<int> count{0};

    auto deadline = std::chrono::steady_clock::now() + 20ms;

    for (int i = 0; i < N; ++i) {
        csp::spawn([&count, deadline] {
            csp::sleep_until(deadline);
            count.fetch_add(1, std::memory_order_relaxed);
        });
    }

    csp::schedule();

    CHECK(N == count.load());

    csp::shutdown_runtime();
}

// ---------------------------------------------------------------------------
// A6. Re-init across shutdown
// ---------------------------------------------------------------------------
// Call set_maxprocs(4), spawn and schedule work, shutdown, then repeat.
// Verify the second cycle works cleanly.

TEST_CASE("Protocol---A6-re-init-across-shutdown") {
    // First cycle.
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    {
        std::atomic<int> count1{0};
        constexpr int N1 = 50;
        for (int i = 0; i < N1; ++i) {
            csp::spawn([&count1] {
                count1.fetch_add(1, std::memory_order_relaxed);
            });
        }
        csp::schedule();
        CHECK(N1 == count1.load());
    }

    csp::shutdown_runtime();

    // Second cycle — re-init via set_maxprocs + schedule.
    csp::set_maxprocs(4);

    {
        std::atomic<int> count2{0};
        constexpr int N2 = 50;
        for (int i = 0; i < N2; ++i) {
            csp::spawn([&count2] {
                count2.fetch_add(1, std::memory_order_relaxed);
            });
        }
        csp::schedule();
        CHECK(N2 == count2.load());
    }

    csp::shutdown_runtime();
}
