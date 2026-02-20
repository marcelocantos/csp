#include "testutil.h"

#include <vector>

using namespace csp;
using namespace csp::part;
using namespace std::chrono_literals;

TEST_SUITE("DeadLetter") {

TEST_CASE("Throttle - excess goes to dead letter") {
    RunStats stats;

    auto [dl_w, dl_r] = chan<int>{};

    // Budget=1, interval=1s. Only first value passes; rest go to dead letter.
    auto r = throttle<int>(tick(1s), {.dead_letter = std::move(dl_w)}).spawn(count(1, 6).spawn());

    std::vector<int> dead;
    stats.spawn([dl_r = std::move(dl_r), &dead]{
        for (int v; dl_r >> v;) dead.push_back(v);
    });

    stats.spawn([r = std::move(r)]{
        CHECK_EQ(1, r.read());
        int _;
        CHECK_FALSE(bool(r >> _));
    });

    csp::schedule();
    CHECK_EQ(std::vector<int>({2, 3, 4, 5}), dead);
}

TEST_CASE("Throttle - no dead letter, no crash") {
    // Same scenario without dead letter — existing behaviour.
    auto r = throttle<int>(tick(1s)).spawn(count(1, 6).spawn());

    CHECK_EQ(1, r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("Throttle - budget reset with dead letter") {
    RunStats stats;

    auto [dl_w, dl_r] = chan<int>{};
    auto th = throttle<int>(tick(100ms), {.n = 2, .dead_letter = std::move(dl_w)}).spawn();

    stats.spawn([w = std::move(th.w)]{
        // First burst: 1,2,3.
        w << 1; w << 2; w << 3;
        // Wait for tick to reset budget.
        csp::sleep(150ms);
        // Second burst: 4,5,6.
        w << 4; w << 5; w << 6;
    });

    std::vector<int> dead;
    stats.spawn([dl_r = std::move(dl_r), &dead]{
        for (int v; dl_r >> v;) dead.push_back(v);
    });

    // First burst: 1,2 pass, 3 to DL.
    // After tick: 4,5 pass, 6 to DL.
    stats.spawn([r = std::move(th.r)]{
        CHECK_EQ(1, r.read());
        CHECK_EQ(2, r.read());
        CHECK_EQ(4, r.read());
        CHECK_EQ(5, r.read());
        int _;
        CHECK_FALSE(bool(r >> _));
    });

    csp::schedule();
    CHECK_EQ(std::vector<int>({3, 6}), dead);
}

TEST_CASE("Debounce - superseded values go to dead letter") {
    RunStats stats;

    auto [dl_w, dl_r] = chan<int>{};

    // count sends 1–5 instantly. Each supersedes the previous pending value.
    // Input closes → pending (5) emitted. 1–4 go to dead letter.
    auto r = debounce<int>(50ms, {.dead_letter = std::move(dl_w)}).spawn(count(1, 6).spawn());

    std::vector<int> dead;
    stats.spawn([dl_r = std::move(dl_r), &dead]{
        for (int v; dl_r >> v;) dead.push_back(v);
    });

    stats.spawn([r = std::move(r)]{
        CHECK_EQ(5, r.read());
        int _;
        CHECK_FALSE(bool(r >> _));
    });

    csp::schedule();
    CHECK_EQ(std::vector<int>({1, 2, 3, 4}), dead);
}

TEST_CASE("Debounce - no dead letter, no crash") {
    auto r = debounce<int>(50ms).spawn(count(1, 6).spawn());

    CHECK_EQ(5, r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("Debounce - spaced values, no drops") {
    RunStats stats;

    auto [dl_w, dl_r] = chan<int>{};
    auto db = debounce<int>(50ms, {.dead_letter = std::move(dl_w)}).spawn();

    stats.spawn([w = std::move(db.w)]{
        w << 1;
        csp::sleep(100ms);
        w << 2;
        csp::sleep(100ms);
        w << 3;
    });

    std::vector<int> dead;
    stats.spawn([dl_r = std::move(dl_r), &dead]{
        for (int v; dl_r >> v;) dead.push_back(v);
    });

    stats.spawn([r = std::move(db.r)]{
        CHECK_EQ(1, r.read());
        CHECK_EQ(2, r.read());
        CHECK_EQ(3, r.read());
        int _;
        CHECK_FALSE(bool(r >> _));
    });

    csp::schedule();
    // No drops — dead letter should be empty.
    CHECK(dead.empty());
}

TEST_CASE("Sample - overwritten values go to dead letter") {
    RunStats stats;

    auto [dl_w, dl_r] = chan<int>{};
    auto [trig_w, trig_r] = chan<>{};
    auto [src_w, src_r] = chan<int>{};
    auto r = sample(std::move(src_r), std::move(trig_r), {.dead_letter = std::move(dl_w)}).spawn();

    std::vector<int> dead;
    stats.spawn([dl_r = std::move(dl_r), &dead]{
        for (int v; dl_r >> v;) dead.push_back(v);
    });

    stats.spawn([src_w = std::move(src_w), trig_w = std::move(trig_w)]{
        // Send 1, 2, 3 then trigger.
        src_w << 1; src_w << 2; src_w << 3;
        trig_w << poke;
    });

    stats.spawn([r = std::move(r)]{
        // Source 1,2,3 all latched; trigger emits latest (3).
        CHECK_EQ(3, r.read());
        int _;
        CHECK_FALSE(bool(r >> _));
    });

    csp::schedule();
    // Dead letter should have 1 and 2 (overwritten by 2 and 3).
    CHECK_EQ(std::vector<int>({1, 2}), dead);
}

TEST_CASE("Sample - no dead letter, no crash") {
    RunStats stats;

    auto [trig_w, trig_r] = chan<>{};
    auto [src_w, src_r] = chan<int>{};
    auto r = sample(std::move(src_r), std::move(trig_r)).spawn();

    stats.spawn([src_w = std::move(src_w), trig_w = std::move(trig_w)]{
        src_w << 1; src_w << 2; src_w << 3;
        trig_w << poke;
    });

    stats.spawn([r = std::move(r)]{
        CHECK_EQ(3, r.read());
        int _;
        CHECK_FALSE(bool(r >> _));
    });

    csp::schedule();
}

}
