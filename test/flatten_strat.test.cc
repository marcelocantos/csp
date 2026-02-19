#include "testutil.h"

#include <algorithm>
#include <vector>

using namespace csp;
using namespace csp::part;

TEST_SUITE("FlattenStrategies") {

// ========================================================================
// merge_all / merge_map
// ========================================================================

TEST_CASE("merge_all - concurrent merge") {
    auto [w, r] = chan<reader<int>>{};
    auto out = merge_all<int>().spawn(std::move(r));

    csp::spawn([w = std::move(w)] {
        w << count(10, 13).spawn();
        w << count(20, 23).spawn();
    });

    std::vector<int> got;
    for (int n; out >> n;) got.push_back(n);
    std::sort(got.begin(), got.end());
    CHECK_EQ(std::vector<int>({10, 11, 12, 20, 21, 22}), got);
}

TEST_CASE("merge_all - empty input") {
    auto [w, r] = chan<reader<int>>{};
    auto out = merge_all<int>().spawn(std::move(r));
    w = {};  // Close input immediately.

    int dummy;
    CHECK_FALSE(bool(out >> dummy));
}

TEST_CASE("merge_all - output death") {
    RunStats stats;

    auto [w, r] = chan<reader<int>>{};
    auto out = merge_all<int>().spawn(std::move(r));

    stats.spawn([w = std::move(w)] {
        w << count_forever(0).spawn();
    });

    out.read();
    out = {};
    csp::schedule();
}

TEST_CASE("merge_map - same as flat_map") {
    auto r = merge_map<int, int>([](int n) {
                 return count(n * 10, n * 10 + 3).spawn();
             })
                 .spawn(count(1, 4).spawn());

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    std::sort(got.begin(), got.end());
    CHECK_EQ(std::vector<int>({10, 11, 12, 20, 21, 22, 30, 31, 32}), got);
}

// ========================================================================
// concat_all / concat_map
// ========================================================================

TEST_CASE("concat_all - sequential drain") {
    auto [w, r] = chan<reader<int>>{};
    auto out = concat_all<int>().spawn(std::move(r));

    csp::spawn([w = std::move(w)] {
        w << count(10, 13).spawn();
        w << count(20, 23).spawn();
    });

    std::vector<int> got;
    for (int n; out >> n;) got.push_back(n);
    // Sequential: sub1 fully drained, then sub2.
    CHECK_EQ(std::vector<int>({10, 11, 12, 20, 21, 22}), got);
}

TEST_CASE("concat_all - empty input") {
    auto [w, r] = chan<reader<int>>{};
    auto out = concat_all<int>().spawn(std::move(r));
    w = {};

    int dummy;
    CHECK_FALSE(bool(out >> dummy));
}

TEST_CASE("concat_all - empty sub-stream") {
    auto [w, r] = chan<reader<int>>{};
    auto out = concat_all<int>().spawn(std::move(r));

    csp::spawn([w = std::move(w)] {
        // Empty sub (closed immediately).
        auto [sw, sr] = chan<int>{};
        w << std::move(sr);
        sw = {};  // Close — sub is empty.
        // Non-empty sub.
        w << count(1, 4).spawn();
    });

    std::vector<int> got;
    for (int n; out >> n;) got.push_back(n);
    CHECK_EQ(std::vector<int>({1, 2, 3}), got);
}

TEST_CASE("concat_map - sequential order") {
    auto r = concat_map<int, int>([](int n) {
                 return count(n * 10, n * 10 + 3).spawn();
             })
                 .spawn(count(1, 4).spawn());

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    CHECK_EQ(std::vector<int>({10, 11, 12, 20, 21, 22, 30, 31, 32}), got);
}

TEST_CASE("concat_map - output death") {
    RunStats stats;

    auto r = concat_map<int, int>([](int n) {
                 return count_forever(n).spawn();
             })
                 .spawn(count_forever(0).spawn());

    for (int i = 0; i < 10; ++i) r.read();
    r = {};
    csp::schedule();
}

// ========================================================================
// switch_all / switch_map
// ========================================================================

TEST_CASE("switch_all - latest wins") {
    auto [outer_w, outer_r] = chan<reader<int>>{};
    auto out = switch_all<int>().spawn(std::move(outer_r));

    csp::spawn([outer_w = std::move(outer_w)] {
        // First sub — manual channel so we control values.
        auto [s1w, s1r] = chan<int>{};
        outer_w << std::move(s1r);
        s1w << 1;  // switch_all reads 1 from sub1.
        // Send second sub — switch_all switches, dropping sub1.
        outer_w << count(10, 13).spawn();
        // s1w drops here — sub1 fully cancelled.
    });

    std::vector<int> got;
    for (int n; out >> n;) got.push_back(n);
    CHECK_EQ(std::vector<int>({1, 10, 11, 12}), got);
}

TEST_CASE("switch_all - sub dies naturally") {
    auto [outer_w, outer_r] = chan<reader<int>>{};
    auto out = switch_all<int>().spawn(std::move(outer_r));

    csp::spawn([outer_w = std::move(outer_w)] {
        // Sub1 — manual channel, fully controlled.
        auto [s1w, s1r] = chan<int>{};
        outer_w << std::move(s1r);
        s1w << 1; s1w << 2; s1w << 3;
        s1w = {};  // Sub1 dies naturally.
        // Yield to let switch_all detect sub1 death before offering sub2.
        csp::yield();
        // Sub2 — accepted after sub1 finishes.
        outer_w << count(10, 13).spawn();
    });

    std::vector<int> got;
    for (int n; out >> n;) got.push_back(n);
    CHECK_EQ(std::vector<int>({1, 2, 3, 10, 11, 12}), got);
}

TEST_CASE("switch_all - empty input") {
    auto [w, r] = chan<reader<int>>{};
    auto out = switch_all<int>().spawn(std::move(r));
    w = {};

    int dummy;
    CHECK_FALSE(bool(out >> dummy));
}

TEST_CASE("switch_map - cancel previous") {
    RunStats stats;

    auto [in_w, in_r] = chan<int>{};
    auto out = switch_map<int, int>([](int n) {
                   return count_forever(n * 10).spawn();
               })
                   .spawn(std::move(in_r));

    stats.spawn([in_w = std::move(in_w)] {
        in_w << 1;  // sub1: 10, 11, 12, ...
        csp::yield();
        csp::yield();
        in_w << 2;  // sub2 cancels sub1: 20, 21, 22, ...
    });

    // Read a few values — should see switch from sub1 to sub2.
    std::vector<int> got;
    for (int i = 0; i < 5; ++i) got.push_back(out.read());
    out = {};
    csp::schedule();

    // First few from sub1, then switch to sub2.
    // The exact transition point depends on scheduling, but
    // the last values must be from sub2 (>= 20).
    CHECK(got.back() >= 20);
}

// ========================================================================
// exhaust_all / exhaust_map
// ========================================================================

TEST_CASE("exhaust_all - discard while draining") {
    auto [outer_w, outer_r] = chan<reader<int>>{};
    auto out = exhaust_all<int>().spawn(std::move(outer_r));

    csp::spawn([outer_w = std::move(outer_w)] {
        // Sub1 — manual channel.
        auto [s1w, s1r] = chan<int>{};
        outer_w << std::move(s1r);

        // While sub1 is active, send sub2 (will be discarded).
        outer_w << count(40, 43).spawn();

        // Now finish sub1.
        s1w << 1;
        s1w << 2;
        s1w << 3;
        s1w = {};  // Close sub1.

        // Sub3 — should be accepted (sub1 is done).
        outer_w << count(70, 73).spawn();
    });

    std::vector<int> got;
    for (int n; out >> n;) got.push_back(n);
    std::string dbg = "exhaust_all discard got:";
    for (int v : got) dbg += " " + std::to_string(v);
    MESSAGE(dbg);
    CHECK_EQ(std::vector<int>({1, 2, 3, 70, 71, 72}), got);
}

TEST_CASE("exhaust_all - no overlap") {
    auto [outer_w, outer_r] = chan<reader<int>>{};
    auto out = exhaust_all<int>().spawn(std::move(outer_r));

    csp::spawn([outer_w = std::move(outer_w)] {
        // Sub1 — manual channel, fully controlled.
        auto [s1w, s1r] = chan<int>{};
        outer_w << std::move(s1r);
        s1w << 1; s1w << 2; s1w << 3;
        s1w = {};  // Sub1 done.
        // Sub2 — no overlap with sub1.
        outer_w << count(10, 13).spawn();
    });

    std::vector<int> got;
    for (int n; out >> n;) got.push_back(n);
    CHECK_EQ(std::vector<int>({1, 2, 3, 10, 11, 12}), got);
}

TEST_CASE("exhaust_all - empty input") {
    auto [w, r] = chan<reader<int>>{};
    auto out = exhaust_all<int>().spawn(std::move(r));
    w = {};

    int dummy;
    CHECK_FALSE(bool(out >> dummy));
}

TEST_CASE("exhaust_map - ignore while busy") {
    RunStats stats;

    auto [in_w, in_r] = chan<int>{};
    auto out = exhaust_map<int, int>([](int n) {
                   return count(n * 10, n * 10 + 3).spawn();
               })
                   .spawn(std::move(in_r));

    stats.spawn([in_w = std::move(in_w)] {
        in_w << 1;  // sub1: 10, 11, 12
        in_w << 2;  // sub2: may be discarded if sub1 is still active
        in_w << 3;  // sub3: accepted after sub1 (and possibly sub2) finishes
    });

    std::vector<int> got;
    for (int n; out >> n;) got.push_back(n);

    // At minimum, sub1 values must be present and in order.
    CHECK(got.size() >= 3);
    CHECK_EQ(10, got[0]);
    CHECK_EQ(11, got[1]);
    CHECK_EQ(12, got[2]);
}

}
