#include "testutil.h"

#include <vector>

using namespace csp;
using namespace csp::part;
using namespace std::chrono_literals;

TEST_SUITE("Pace") {

TEST_CASE("Pace - all values pass through") {
    RunStats stats;

    auto p = pace<int>(tick(50ms)).spawn();

    stats.spawn([w = std::move(p.w)]{
        w << 1; w << 2; w << 3;
    });

    std::vector<int> got;
    stats.spawn([r = std::move(p.r), &got]{
        for (int v; r >> v;) got.push_back(v);
    });

    csp::schedule();
    CHECK_EQ(std::vector<int>({1, 2, 3}), got);
}

TEST_CASE("Pace - enforces minimum interval") {
    RunStats stats;

    auto p = pace<int>(tick(80ms)).spawn();

    stats.spawn([w = std::move(p.w)]{
        w << 1; w << 2; w << 3;
    });

    std::vector<csp::clock::time_point> times;
    stats.spawn([r = std::move(p.r), &times]{
        for (int v; r >> v;) {
            (void)v;
            times.push_back(csp::clock::now());
        }
    });

    csp::schedule();
    REQUIRE_EQ(3u, times.size());
    // First → second and second → third should each be >= 80ms.
    CHECK_GE(times[1] - times[0], 75ms);  // small tolerance
    CHECK_GE(times[2] - times[1], 75ms);
}

TEST_CASE("Pace - first value passes immediately") {
    RunStats stats;

    auto p = pace<int>(tick(200ms)).spawn();
    auto start = csp::clock::now();

    stats.spawn([w = std::move(p.w)]{
        w << 42;
    });

    csp::clock::time_point received;
    stats.spawn([r = std::move(p.r), &received]{
        int v;
        r >> v;
        CHECK_EQ(42, v);
        received = csp::clock::now();
    });

    csp::schedule();
    CHECK_LT(received - start, 50ms);  // should be near-instant
}

TEST_CASE("Pace - output death stops") {
    RunStats stats;

    auto p = pace<int>(tick(50ms)).spawn();

    stats.spawn([w = std::move(p.w)]{
        // Send many values — output will die after first read.
        for (int i = 0; i < 100; ++i) w << i;
    });

    stats.spawn([r = std::move(p.r)]{
        CHECK_EQ(0, r.read());
        // Drop reader — output dies.
    });

    csp::schedule();
}

TEST_CASE("Pace - pipe composition") {
    RunStats stats;

    auto r = count(1, 4).spawn() | pace<int>(tick(50ms));

    std::vector<int> got;
    stats.spawn([r = std::move(r), &got]{
        for (int v; r >> v;) got.push_back(v);
    });

    csp::schedule();
    CHECK_EQ(std::vector<int>({1, 2, 3}), got);
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
        CHECK_EQ(1, r.read());
        int _;
        CHECK_FALSE(bool(r >> _));
    });

    csp::schedule();
}

}
