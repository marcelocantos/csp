// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest/doctest.h>

#if __has_include(<csp/internal/note.h>)
#include <csp/internal/note.h>
#else
#include "csp.h"
#endif

#include <atomic>
#include <barrier>
#include <chrono>
#include <limits>
#include <thread>

TEST_SUITE("Note") {

TEST_CASE("wake-before-sleep-is-retained") {
    csp::detail::Note note;
    note.wake();
    CHECK(note.sleep_for(std::chrono::milliseconds(100)));
}

TEST_CASE("wake-racing-sleep-entry-is-not-lost") {
    using clock = std::chrono::steady_clock;
    using ticks = clock::duration::rep;
    constexpr int kAttempts = 10000;
    constexpr auto kWait = std::chrono::milliseconds(100);
    constexpr auto kNotWoken = std::numeric_limits<ticks>::max();

    csp::detail::Note note;
    std::barrier phase(2);
    std::atomic<ticks> wake_finished{kNotWoken};
    std::atomic<bool> lost_wake{false};
    std::thread sleeper([&] {
        for (int i = 0; i < kAttempts; ++i) {
            wake_finished.store(kNotWoken, std::memory_order_relaxed);
            phase.arrive_and_wait();
            auto deadline = clock::now() + kWait;
            bool woken = note.sleep_for(kWait);
            // A late OS scheduling slice is not a lost wake. Only fail if
            // wake() demonstrably returned before this wait's deadline.
            if (!woken && wake_finished.load(std::memory_order_acquire)
                              < deadline.time_since_epoch().count()) {
                lost_wake.store(true, std::memory_order_relaxed);
            }
            phase.arrive_and_wait();
            if (lost_wake.load(std::memory_order_relaxed)) break;
        }
    });
    for (int i = 0; i < kAttempts; ++i) {
        phase.arrive_and_wait();
        note.wake();
        wake_finished.store(clock::now().time_since_epoch().count(),
                            std::memory_order_release);
        phase.arrive_and_wait();
        if (lost_wake.load(std::memory_order_relaxed)) break;
    }
    sleeper.join();
    CHECK_FALSE(lost_wake.load());
}

} // TEST_SUITE
