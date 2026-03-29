#include "testutil.h"
#include "testscale.h"

#include <vector>

using namespace csp;
using namespace csp::part;
using namespace std::chrono_literals;

TEST_SUITE("Pace") {

TEST_CASE("Pace - all values pass through") {
    RunStats stats;
    fake_clock fc;

    std::vector<int> got;

    stats.spawn([&]{
        csp::local l{csp::clock = &fc};
        auto p = pace<int>(tick(50ms)).spawn();

        csp::spawn([w = std::move(p.w)]{
            w << 1; w << 2; w << 3;
        });

        for (int v; p.r >> v;) got.push_back(v);
    });

    fc.run();
    CHECK(std::vector<int>({1, 2, 3}) == got);
}

TEST_CASE("Pace - enforces minimum interval") {
    RunStats stats;
    fake_clock fc;
    auto epoch = time_point{};

    std::vector<csp::time_point> times;

    stats.spawn([&]{
        csp::local l{csp::clock = &fc};
        auto p = pace<int>(tick(80ms)).spawn();

        csp::spawn([w = std::move(p.w)]{
            w << 1; w << 2; w << 3;
        });

        for (int v; p.r >> v;) {
            (void)v;
            times.push_back(csp::now());
        }
    });

    fc.run();
    REQUIRE(3u == times.size());
    CHECK(times[0] == epoch);
    CHECK(times[1] == epoch + 80ms);
    CHECK(times[2] == epoch + 160ms);
}

TEST_CASE("Pace - first value passes immediately") {
    RunStats stats;
    fake_clock fc;
    auto epoch = time_point{};

    csp::time_point received;

    stats.spawn([&]{
        csp::local l{csp::clock = &fc};
        auto p = pace<int>(tick(200ms)).spawn();

        csp::spawn([w = std::move(p.w)]{
            w << 42;
        });

        int v;
        p.r >> v;
        CHECK(42 == v);
        received = csp::now();
    });

    fc.run();
    CHECK(received == epoch);
}

TEST_CASE("Pace - output death stops") {
    RunStats stats;
    fake_clock fc;

    stats.spawn([&]{
        csp::local l{csp::clock = &fc};
        auto p = pace<int>(tick(50ms)).spawn();

        csp::spawn([w = std::move(p.w)]{
            // Send many values — output will die after first read.
            for (int i = 0; i < 100; ++i) w << i;
        });

        CHECK(0 == p.r.read());
        // Drop reader — output dies.
    });

    fc.run();
}

TEST_CASE("Pace - pipe composition") {
    RunStats stats;
    fake_clock fc;

    std::vector<int> got;

    stats.spawn([&]{
        csp::local l{csp::clock = &fc};
        auto r = count(1, 4).spawn() | pace<int>(tick(50ms));

        for (int v; r >> v;) got.push_back(v);
    });

    fc.run();
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
