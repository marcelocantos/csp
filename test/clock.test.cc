#include "testutil.h"

#include <doctest/doctest.h>

#include <chrono>
#include <vector>

using namespace csp;
using namespace std::chrono_literals;

TEST_CASE("now() returns real time without override") {
    RunStats stats;

    clock::time_point t;
    stats.spawn([&] {
        auto before = clock::now();
        t = csp::now();
        auto after = clock::now();
        CHECK_GE(t, before);
        CHECK_LE(t, after);
    });
    csp::schedule();
}

TEST_CASE("now() returns fake time when overridden") {
    RunStats stats;

    fake_clock fc;
    stats.spawn([&] {
        csp::local l{clock_override = &fc};
        CHECK_EQ(csp::now(), fc.now());
        fc.advance(1s);
        CHECK_EQ(csp::now(), fc.now());
    });
    csp::schedule();
}

TEST_CASE("advance moves time forward") {
    fake_clock fc;
    auto start = fc.now();
    fc.advance(500ms);
    CHECK_EQ(fc.now(), start + 500ms);
    fc.advance(500ms);
    CHECK_EQ(fc.now(), start + 1s);
}

TEST_CASE("sleep with fake clock") {
    RunStats stats;

    fake_clock fc;
    bool woke = false;

    stats.spawn([&] {
        csp::local l{clock_override = &fc};
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
    clock::time_point fired_at{};

    stats.spawn([&] {
        csp::local l{clock_override = &fc};
        auto r = csp::after(1s);
        r >> fired_at;
    });
    csp::schedule();

    CHECK(fired_at == clock::time_point{});
    CHECK(fc.has_pending());

    fc.advance(999ms);
    while (csp::internal::run()) {}
    CHECK(fired_at == clock::time_point{});

    fc.advance(1ms);
    while (csp::internal::run()) {}
    CHECK(fired_at != clock::time_point{});
    CHECK_EQ(fired_at, fc.now());
}

TEST_CASE("tick fires periodically") {
    RunStats stats;

    fake_clock fc;
    std::vector<clock::time_point> ticks;

    stats.spawn([&] {
        csp::local l{clock_override = &fc};
        auto r = csp::tick(100ms);
        for (int i = 0; i < 3; ++i) {
            clock::time_point t;
            r >> t;
            ticks.push_back(t);
        }
    });
    csp::schedule();

    // Auto-advance fires 3 ticks for the reader, then a 4th wakes the
    // producer which discovers the dead reader and exits cleanly.
    fc.run();

    REQUIRE_EQ(ticks.size(), 3);
    auto epoch = clock::time_point{};
    CHECK_EQ(ticks[0], epoch + 100ms);
    CHECK_EQ(ticks[1], epoch + 200ms);
    CHECK_EQ(ticks[2], epoch + 300ms);
}

TEST_CASE("advance_to_next jumps to exact deadline") {
    RunStats stats;

    fake_clock fc;
    bool woke = false;

    stats.spawn([&] {
        csp::local l{clock_override = &fc};
        stats.spawn([&] {
            csp::sleep(42ms);
            woke = true;
        });
    });
    csp::schedule();

    CHECK(!woke);
    CHECK(fc.advance_to_next());
    CHECK_EQ(fc.now(), clock::time_point{} + 42ms);
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
        csp::local l{clock_override = &fc};
        stats.spawn([&] { csp::sleep(300ms); order.push_back(3); });
        stats.spawn([&] { csp::sleep(100ms); order.push_back(1); });
        stats.spawn([&] { csp::sleep(200ms); order.push_back(2); });
    });
    csp::schedule();

    while (fc.advance_to_next()) {
        while (csp::internal::run()) {}
    }

    CHECK_EQ(order, std::vector{1, 2, 3});
}

TEST_CASE("auto-advance via run()") {
    RunStats stats;

    fake_clock fc;
    std::vector<int> order;

    stats.spawn([&] {
        csp::local l{clock_override = &fc};
        stats.spawn([&] { csp::sleep(300ms); order.push_back(3); });
        stats.spawn([&] { csp::sleep(100ms); order.push_back(1); });
        stats.spawn([&] { csp::sleep(200ms); order.push_back(2); });
    });
    csp::schedule();

    fc.run();

    CHECK_EQ(order, std::vector{1, 2, 3});
}

TEST_CASE("run_until_idle does not advance time") {
    RunStats stats;

    fake_clock fc;
    bool woke = false;

    stats.spawn([&] {
        csp::local l{clock_override = &fc};
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
    clock::time_point child_time{};

    stats.spawn([&] {
        csp::local l{clock_override = &fc};
        fc.advance(42s);
        stats.spawn([&] {
            // Child should see the fake clock via dynamic scoping.
            child_time = csp::now();
        });
    });
    csp::schedule();
    while (csp::internal::run()) {}

    CHECK_EQ(child_time, clock::time_point{} + 42s);
}

TEST_CASE("sleep_until past time returns immediately") {
    RunStats stats;

    fake_clock fc;
    fc.advance(1s);
    bool completed = false;

    stats.spawn([&] {
        csp::local l{clock_override = &fc};
        // sleep_until a time in the past — should not suspend.
        csp::sleep_until(clock::time_point{} + 500ms);
        completed = true;
    });
    csp::schedule();

    CHECK(completed);
    CHECK(!fc.has_pending());
}
