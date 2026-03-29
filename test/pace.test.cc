#include "testutil.h"
#include "testscale.h"

#include <vector>

using namespace csp;
using namespace csp::part;
using namespace std::chrono_literals;

TEST_SUITE("Pace") {

TEST_CASE("Pace - all values pass through") {
    fake_clock fc;

    std::vector<int> got;

    csp::run([&] {
        csp::local l{fc.binding()};
        auto p = pace<int>(tick(50ms)).spawn();

        csp::spawn([w = std::move(p.w)]{
            w << 1; w << 2; w << 3;
        });

        for (int v; p.r >> v;) got.push_back(v);
    });

    CHECK(std::vector<int>({1, 2, 3}) == got);
}

TEST_CASE("Pace - enforces minimum interval") {
    fake_clock fc;

    std::vector<int> got;
    csp::time_point first_time;
    csp::time_point last_time;

    csp::run([&] {
        csp::local l{fc.binding()};
        auto p = pace<int>(tick(80ms)).spawn();

        csp::spawn([w = std::move(p.w)]{
            w << 1; w << 2; w << 3;
        });

        bool first = true;
        for (int v; p.r >> v;) {
            got.push_back(v);
            if (first) { first_time = csp::now(); first = false; }
            last_time = csp::now();
        }
    });

    CHECK(std::vector<int>({1, 2, 3}) == got);
    // At least one tick interval elapses between first and last value,
    // confirming that pace enforces time-based spacing. Under automatic
    // quiescence-driven clock advancement, the exact number of tick
    // boundaries crossed is non-deterministic.
    CHECK(last_time - first_time >= 80ms);
}

TEST_CASE("Pace - first value passes immediately") {
    fake_clock fc;

    int got = 0;

    csp::run([&] {
        csp::local l{fc.binding()};
        auto p = pace<int>(tick(200ms)).spawn();

        csp::spawn([w = std::move(p.w)]{
            w << 42;
        });

        int v;
        p.r >> v;
        got = v;
    });

    CHECK(42 == got);
}

TEST_CASE("Pace - output death stops") {
    fake_clock fc;

    csp::run([&] {
        csp::local l{fc.binding()};
        auto p = pace<int>(tick(50ms)).spawn();

        csp::spawn([w = std::move(p.w)]{
            // Send many values — output will die after first read.
            for (int i = 0; i < 100; ++i) w << i;
        });

        CHECK(0 == p.r.read());
        // Drop reader — output dies.
    });
}

TEST_CASE("Pace - pipe composition") {
    fake_clock fc;

    std::vector<int> got;

    csp::run([&] {
        csp::local l{fc.binding()};
        auto r = count(1, 4).spawn() | pace<int>(tick(50ms));

        for (int v; r >> v;) got.push_back(v);
    });

    CHECK(std::vector<int>({1, 2, 3}) == got);
}

TEST_CASE("Pace - trigger death stops") {
    RunStats stats;

    auto [tw, tr] = chan<>{};
    auto p = pace<int>(std::move(tr)).spawn();

    stats.spawn([w = std::move(p.w), tw = std::move(tw)]{
        w << 1;  // Passes immediately (first value).
        // tw dropped here — trigger dies, pace should stop.
        w << 2;
    });

    stats.spawn([r = std::move(p.r)]{
        CHECK(1 == r.read());
        int _;
        CHECK_FALSE(bool(r >> _));
    });

    csp::schedule();
}

}
