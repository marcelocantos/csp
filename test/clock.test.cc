#include "testutil.h"

#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <vector>

using namespace csp;
using namespace csp::part;
using namespace std::chrono_literals;

TEST_CASE("now() returns real time without override") {
    RunStats stats;

    time_point t;
    stats.spawn([&] {
        auto before = std::chrono::steady_clock::now();
        t = csp::now();
        auto after = std::chrono::steady_clock::now();
        CHECK(t >= before);
        CHECK(t <= after);
    });
    csp::schedule();
}

TEST_CASE("now() returns fake time when overridden") {
    RunStats stats;

    fake_clock fc;
    stats.spawn([&] {
        csp::local l{csp::clock = &fc};
        CHECK(csp::now() == fc.now());
        fc.advance(1s);
        CHECK(csp::now() == fc.now());
    });
    csp::schedule();
}

TEST_CASE("advance moves time forward") {
    fake_clock fc;
    auto start = fc.now();
    fc.advance(500ms);
    CHECK(fc.now() == start + 500ms);
    fc.advance(500ms);
    CHECK(fc.now() == start + 1s);
}

TEST_CASE("sleep with fake clock") {
    RunStats stats;

    fake_clock fc;
    bool woke = false;

    stats.spawn([&] {
        csp::local l{csp::clock = &fc};
        stats.spawn([&] {
            csp::sleep(100ms);
            woke = true;
        });
    });
    csp::schedule();

    // Imp is now sleeping in the fake clock's queue.
    CHECK(!woke);
    CHECK(fc.has_pending());

    fc.advance(50ms);
    while (csp::internal::run()) {}
    CHECK(!woke);

    fc.advance(50ms);
    while (csp::internal::run()) {}
    CHECK(woke);
}

TEST_CASE("after fires on advance") {
    RunStats stats;

    fake_clock fc;
    time_point fired_at{};

    stats.spawn([&] {
        csp::local l{csp::clock = &fc};
        auto r = csp::after(1s);
        r >> fired_at;
    });
    csp::schedule();

    CHECK(fired_at == time_point{});
    CHECK(fc.has_pending());

    fc.advance(999ms);
    while (csp::internal::run()) {}
    CHECK(fired_at == time_point{});

    fc.advance(1ms);
    while (csp::internal::run()) {}
    CHECK(fired_at != time_point{});
    CHECK(fired_at == fc.now());
}

TEST_CASE("tick fires periodically") {
    RunStats stats;

    fake_clock fc;
    std::vector<time_point> ticks;

    stats.spawn([&] {
        csp::local l{csp::clock = &fc};
        auto r = csp::tick(100ms);
        for (int i = 0; i < 3; ++i) {
            time_point t;
            r >> t;
            ticks.push_back(t);
        }
    });
    csp::schedule();

    // Auto-advance fires 3 ticks for the reader, then a 4th wakes the
    // producer which discovers the dead reader and exits cleanly.
    fc.run();

    REQUIRE(ticks.size() == 3);
    auto epoch = time_point{};
    CHECK(ticks[0] == epoch + 100ms);
    CHECK(ticks[1] == epoch + 200ms);
    CHECK(ticks[2] == epoch + 300ms);
}

TEST_CASE("advance_to_next jumps to exact deadline") {
    RunStats stats;

    fake_clock fc;
    bool woke = false;

    stats.spawn([&] {
        csp::local l{csp::clock = &fc};
        stats.spawn([&] {
            csp::sleep(42ms);
            woke = true;
        });
    });
    csp::schedule();

    CHECK(!woke);
    CHECK(fc.advance_to_next());
    CHECK(fc.now() == time_point{} + 42ms);
    while (csp::internal::run()) {}
    CHECK(woke);
}

TEST_CASE("advance_to_next returns false when empty") {
    fake_clock fc;
    CHECK(!fc.advance_to_next());
}

TEST_CASE("multiple timers fire in order") {
    RunStats stats;

    fake_clock fc;
    std::vector<int> order;

    stats.spawn([&] {
        csp::local l{csp::clock = &fc};
        stats.spawn([&] { csp::sleep(300ms); order.push_back(3); });
        stats.spawn([&] { csp::sleep(100ms); order.push_back(1); });
        stats.spawn([&] { csp::sleep(200ms); order.push_back(2); });
    });
    csp::schedule();

    while (fc.advance_to_next()) {
        while (csp::internal::run()) {}
    }

    CHECK(order == std::vector{1, 2, 3});
}

TEST_CASE("auto-advance via run()") {
    RunStats stats;

    fake_clock fc;
    std::vector<int> order;

    stats.spawn([&] {
        csp::local l{csp::clock = &fc};
        stats.spawn([&] { csp::sleep(300ms); order.push_back(3); });
        stats.spawn([&] { csp::sleep(100ms); order.push_back(1); });
        stats.spawn([&] { csp::sleep(200ms); order.push_back(2); });
    });
    csp::schedule();

    fc.run();

    CHECK(order == std::vector{1, 2, 3});
}

TEST_CASE("run_until_idle does not advance time") {
    RunStats stats;

    fake_clock fc;
    bool woke = false;

    stats.spawn([&] {
        csp::local l{csp::clock = &fc};
        stats.spawn([&] {
            csp::sleep(100ms);
            woke = true;
        });
    });
    csp::schedule();

    fc.run_until_idle();
    CHECK(!woke);  // Timer hasn't fired — time didn't advance.

    fc.advance(100ms);
    fc.run_until_idle();
    CHECK(woke);
}

TEST_CASE("child inherits fake clock") {
    RunStats stats;

    fake_clock fc;
    time_point child_time{};

    stats.spawn([&] {
        csp::local l{csp::clock = &fc};
        fc.advance(42s);
        stats.spawn([&] {
            // Child should see the fake clock via dynamic scoping.
            child_time = csp::now();
        });
    });
    csp::schedule();
    while (csp::internal::run()) {}

    CHECK(child_time == time_point{} + 42s);
}

TEST_CASE("sleep_until past time returns immediately") {
    RunStats stats;

    fake_clock fc;
    fc.advance(1s);
    bool completed = false;

    stats.spawn([&] {
        csp::local l{csp::clock = &fc};
        // sleep_until a time in the past — should not suspend.
        csp::sleep_until(time_point{} + 500ms);
        completed = true;
    });
    csp::schedule();

    CHECK(completed);
    CHECK(!fc.has_pending());
}

// ---- Demo: fake clock accelerates real combinator tests ----

TEST_CASE("fake clock: debounce emits after quiet period") {
    // Without fake clock this would need real 200ms waits.
    // With fake clock it's instant and deterministic.
    RunStats stats;

    fake_clock fc;
    std::vector<int> out;

    stats.spawn([&] {
        csp::local l{csp::clock = &fc};
        auto ch = chan<int>{};
        auto result = std::move(ch.r) | debounce<int>(200ms);
        stats.spawn([&out, result = std::move(result)] {
            for (int v : result) out.push_back(v);
        });

        // Rapid fire — only the last value in each burst should survive.
        ch.w << 1;
        ch.w << 2;
        ch.w << 3;  // Only this one should emit (supersedes 1 and 2).
    });
    csp::schedule();
    fc.run();

    CHECK(out == std::vector{3});
}

TEST_CASE("fake clock: timeout closes after inactivity") {
    // Tests a 5-second timeout without waiting 5 real seconds.
    RunStats stats;

    fake_clock fc;
    std::vector<int> out;

    stats.spawn([&] {
        csp::local l{csp::clock = &fc};
        auto ch = chan<int>{};

        auto result = std::move(ch.r) | timeout<int>(5s);
        stats.spawn([&out, result = std::move(result)] {
            for (int v : result) out.push_back(v);
        });

        // Send a value, then let time expire.
        ch.w << 42;
    });
    csp::schedule();
    fc.run();

    CHECK(out == std::vector{42});
    // The timeout fired at fake time = 5s, closing the pipeline.
    // Total real time: ~0ms.
}

TEST_CASE("fake clock: delay shifts values forward in time") {
    // Tests 1-second delays without waiting.
    RunStats stats;

    fake_clock fc;
    std::vector<int> out;

    stats.spawn([&] {
        csp::local l{csp::clock = &fc};
        auto ch = chan<int>{};

        auto result = std::move(ch.r) | delay<int>(1s);
        stats.spawn([&out, result = std::move(result)] {
            for (int v : result) out.push_back(v);
        });

        ch.w << 10;
        ch.w << 20;
        ch.w << 30;
    });
    csp::schedule();

    // All values were written at t=0, so all have deadline t=1s.
    CHECK(out.empty());

    fc.advance(500ms);
    fc.run_until_idle();
    CHECK(out.empty());  // Still waiting.

    fc.advance(500ms);  // Now at 1s — all deadlines hit.
    fc.run();
    CHECK(out == std::vector{10, 20, 30});
}

TEST_CASE("fake clock: throttle rate-limits with tick trigger") {
    // Tests rate-limiting at "1 per 100ms" without real delays.
    RunStats stats;

    fake_clock fc;
    std::vector<int> out;

    stats.spawn([&] {
        csp::local l{csp::clock = &fc};
        auto ch = chan<int>{};

        auto result = std::move(ch.r) | throttle<int>(tick(100ms));
        stats.spawn([&out, result = std::move(result)] {
            for (int v : result) out.push_back(v);
        });

        // Send 3 values immediately — only the first should pass
        // (budget = 1 per tick, budget starts at 1).
        ch.w << 1;
        ch.w << 2;
        ch.w << 3;
    });
    csp::schedule();

    // First value passed (initial budget = 1), rest dropped.
    CHECK(out == std::vector{1});

    // Advance one tick — budget resets. But no more input, so nothing new.
    fc.run();
}

TEST_CASE("after with zero duration fires promptly") {
    RunStats stats;

    fake_clock fc;
    bool fired = false;

    stats.spawn([&] {
        csp::local l{csp::clock = &fc};
        auto r = csp::after(0ns);
        r >> nullptr;
        fired = true;
    });
    csp::schedule();
    fc.run();

    CHECK(fired);
}

TEST_CASE("sleep with zero duration returns immediately") {
    RunStats stats;

    fake_clock fc;
    bool completed = false;

    stats.spawn([&] {
        csp::local l{csp::clock = &fc};
        csp::sleep(0ns);
        completed = true;
    });
    csp::schedule();
    // Zero sleep should complete without needing fc.run().
    // But drain any residual work just in case.
    while (csp::internal::run()) {}

    CHECK(completed);
}

TEST_CASE("sleep_until exact now returns immediately") {
    RunStats stats;

    fake_clock fc;
    fc.advance(1s);
    bool completed = false;

    stats.spawn([&] {
        csp::local l{csp::clock = &fc};
        csp::sleep_until(csp::now());  // deadline == current time
        completed = true;
    });
    csp::schedule();
    while (csp::internal::run()) {}

    CHECK(completed);
    CHECK(!fc.has_pending());
}

TEST_CASE("fake_clock with non-default start time") {
    auto epoch = time_point{};
    auto start = epoch + 42s;
    fake_clock fc(start);
    CHECK(fc.now() == start);

    fc.advance(10s);
    CHECK(fc.now() == start + 10s);
}

TEST_CASE("fake clock: periodic polling with exact timing") {
    // Simulates polling a sensor every 500ms for 2s.
    // With real time: 2s wait. With fake clock: instant.
    RunStats stats;

    fake_clock fc;
    int poll_count = 0;
    std::vector<time_point> poll_times;

    stats.spawn([&] {
        csp::local l{csp::clock = &fc};
        auto ticker = tick(500ms);
        for (int i = 0; i < 4; ++i) {
            time_point tp;
            ticker >> tp;
            poll_count++;
            poll_times.push_back(tp);
        }
    });
    csp::schedule();
    fc.run();

    CHECK(poll_count == 4);

    auto epoch = time_point{};
    REQUIRE(poll_times.size() == 4);
    CHECK(poll_times[0] == epoch + 500ms);
    CHECK(poll_times[1] == epoch + 1s);
    CHECK(poll_times[2] == epoch + 1500ms);
    CHECK(poll_times[3] == epoch + 2s);
}
