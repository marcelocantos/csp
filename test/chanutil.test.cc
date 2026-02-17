#include "testutil.h"

#include <csp/part/batch.h>
#include <csp/part/blackhole.h>
#include <csp/part/chain.h>
#include <csp/part/count.h>
#include <csp/part/deaf.h>
#include <csp/part/enumerate.h>
#include <csp/part/killswitch.h>
#include <csp/part/latch.h>
#include <csp/part/map.h>
#include <csp/part/merge.h>
#include <csp/part/mute.h>
#include <csp/part/scan.h>
#include <csp/part/sink.h>
#include <csp/part/tee.h>
#include <csp/part/where.h>
#include <csp/part/zip.h>

using namespace csp;
using namespace csp::part;

TEST_CASE("ChanUtil - Batch") {
    // 10 elements in batches of 3 → [1,2,3], [4,5,6], [7,8,9], [10]
    auto r = batch<int>(3).spawn(count(1, 11).spawn());

    auto v = r.read();
    CHECK_EQ(3, v.size());
    CHECK_EQ(1, v[0]); CHECK_EQ(2, v[1]); CHECK_EQ(3, v[2]);

    v = r.read();
    CHECK_EQ(3, v.size());
    CHECK_EQ(4, v[0]); CHECK_EQ(5, v[1]); CHECK_EQ(6, v[2]);

    v = r.read();
    CHECK_EQ(3, v.size());
    CHECK_EQ(7, v[0]); CHECK_EQ(8, v[1]); CHECK_EQ(9, v[2]);

    // Partial final batch.
    v = r.read();
    CHECK_EQ(1, v.size());
    CHECK_EQ(10, v[0]);

    std::vector<int> _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Batch exact") {
    // 6 elements in batches of 3 → two full batches, no partial.
    auto r = batch<int>(3).spawn(count(1, 7).spawn());

    auto v = r.read();
    CHECK_EQ(3, v.size());
    CHECK_EQ(1, v[0]);

    v = r.read();
    CHECK_EQ(3, v.size());
    CHECK_EQ(4, v[0]);

    std::vector<int> _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Blackhole") {
    auto w = blackhole<int>.spawn();

    for (int i = 0; i < 1000; ++i) {
        w << i;
    }
}

TEST_CASE("ChanUtil - Chain") {
    std::vector<reader<int>> v1;
    v1.push_back(count(0, 10).spawn());
    v1.push_back(count(10, 20).spawn());
    auto a = chain<int>(std::move(v1)).spawn();

    std::vector<reader<int>> v2;
    v2.push_back(count(20, 30).spawn());
    v2.push_back(count(30, 40).spawn());
    auto b = chain<int>(std::move(v2)).spawn();

    std::vector<reader<int>> v3;
    v3.push_back(std::move(a));
    v3.push_back(std::move(b));
    auto c = chain<int>(std::move(v3)).spawn();

    for (int i = 0, n; c >> n; ++i) {
        CHECK_EQ(i, n);
    }
}

TEST_CASE("ChanUtil - Count") {
    RunStats stats;

    auto e = count(2, 12345, 7).spawn();

    for (int i = 2, n; e >> n; i += 7) {
        CHECK_EQ(i, n);
    }
}

TEST_CASE("ChanUtil - CountCyclic") {
    RunStats stats;

    auto e = count(2, 15, 7, true).spawn();

    for (int i = 0; i < 100; i += 7) {
        CHECK_EQ(2 + i % (15 - 2), e.read());
    }
}

TEST_CASE("ChanUtil - CountForever") {
    RunStats stats;

    auto e = count_forever(2, 11).spawn();

    for (int i = 2, n; i < 10000; i += 11) {
        CHECK(bool(e >> n));
        CHECK_EQ(i, n);
    }
}

TEST_CASE("ChanUtil - Deaf") {
    RunStats stats;

    auto w = deaf<int>.spawn();
    auto [give_up_w, give_up_r] = chan<>{};

    stats.spawn([w = std::move(w), give_up = std::move(give_up_r)]{
        CHECK_EQ(-2, prialt(w << 42, ~give_up));
    });

    while (csp::internal::run()) { }
    give_up_w = {};
    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - Enumerate") {
    RunStats stats;

    reader<int> e = cycle<int>({2, 3, 5}).spawn();

    int product = 1;

    for (int i = 0; i < 4; ++i) {
        product *= e.read();
    }

    e = {};
    while (csp::internal::run()) { }

    CHECK_EQ(2 * 3 * 5 * 2, product);
}

TEST_CASE("ChanUtil - KillSwitch") {
    RunStats stats;

    auto [keepalive_w, keepalive_r] = chan<>{};
    auto ks = killswitch<int>(std::move(keepalive_r)).spawn();

    CHECK(bool(ks.w.copy() << 42));
    CHECK_EQ(42, ks.r.copy().read());

    keepalive_w = {};
    CHECK_FALSE((ks.w.copy() << 21));
    int _;
    CHECK_FALSE((ks.r.copy() >> _));
}

TEST_CASE("ChanUtil - Latch") {
    RunStats stats;

    auto lat = latch<int>.spawn();

    stats.spawn([in = lat.r.copy()]{
        CHECK_EQ(1, in.read());
    });

    while (csp::internal::run()) { }

    stats.spawn([out = std::move(lat.w)]{
        for (int n = 1; n <= 5; ++n) {
            out << n;
        }
    });

    while (csp::internal::run()) { }

    stats.spawn([in = std::move(lat.r)]{
        CHECK_EQ(5, in.read());
    });

    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - Map") {
    RunStats stats;

    auto plus_one = map<int>([](int n) { return n + 1; }).spawn();

    stats.spawn([out = std::move(plus_one.w)]{
        out << 41;
    });

    stats.spawn([in = std::move(plus_one.r)]{
        CHECK_EQ(42, in.read());
    });

    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - MapStrToLen") {
    RunStats stats;

    auto [words_w, words_r] = chan<std::string>{};
    auto [lengths_w, lengths_r] = chan<size_t>{};
    spawn(map<std::string, size_t>([](auto && s) { return s.length(); })
        .bind(std::move(words_r), std::move(lengths_w)));

    stats.spawn([out = std::move(words_w)]{
        std::string message[] = {"The", "rain", "in", "spain", "falls", "mainly", "on", "the", "plain"};
        for (auto const & word : message) {
            out << word;
        }
    });

    for (size_t i : {3, 4, 2, 5, 5, 6, 2, 3, 5}) {
        CHECK_EQ(i, lengths_r.read());
    }

    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - Merge") {
    // Merge three count streams. All 15 values should arrive (order varies).
    std::vector<reader<int>> rs;
    rs.push_back(count(0, 5).spawn());
    rs.push_back(count(10, 15).spawn());
    rs.push_back(count(20, 25).spawn());
    auto r = merge(std::move(rs)).spawn();

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    CHECK_EQ(15, got.size());

    std::sort(got.begin(), got.end());
    std::vector<int> expect = {0,1,2,3,4, 10,11,12,13,14, 20,21,22,23,24};
    CHECK_EQ(expect, got);
}

TEST_CASE("ChanUtil - Merge single") {
    std::vector<reader<int>> rs;
    rs.push_back(count(1, 4).spawn());
    auto r = merge(std::move(rs)).spawn();

    CHECK_EQ(1, r.read());
    CHECK_EQ(2, r.read());
    CHECK_EQ(3, r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Merge output death") {
    RunStats stats;

    std::vector<reader<int>> rs;
    rs.push_back(count_forever(0).spawn());
    rs.push_back(count_forever(0).spawn());
    auto r = merge(std::move(rs)).spawn();

    // Read a few then drop — merge should terminate.
    for (int i = 0; i < 10; ++i) r.read();
    r = {};
    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - Scan") {
    // Running sum: 1, 1+2, 1+2+3, 1+2+3+4, 1+2+3+4+5
    auto r = scan<int, int>(0, [](int acc, int v) { return acc + v; })
                 .spawn(count(1, 6).spawn());

    CHECK_EQ(1, r.read());
    CHECK_EQ(3, r.read());
    CHECK_EQ(6, r.read());
    CHECK_EQ(10, r.read());
    CHECK_EQ(15, r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Scan type change") {
    // Accumulate string lengths into an int.
    auto r = scan<std::string, int>(0, [](int acc, std::string s) { return acc + (int)s.size(); })
                 .spawn(enumerate<std::string>({"ab", "cde", "f"}).spawn());

    CHECK_EQ(2, r.read());
    CHECK_EQ(5, r.read());
    CHECK_EQ(6, r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Mute") {
    RunStats stats;

    auto r = mute<int>.spawn();
    auto [give_up_w, give_up_r] = chan<>{};

    stats.spawn([r = r.copy(), give_up = std::move(give_up_r)]{
        int n;
        CHECK_GT(0, prialt(r >> n, ~give_up));
    });

    while (csp::internal::run()) { }
    give_up_w = {};
    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - Sink") {
    RunStats stats;

    int total = 0;

    auto snk = sink<int>([&](int n) { total += n; }).spawn();

    for (int i = 1; i <= 10; ++i) {
        snk << i;
    }

    CHECK_EQ(55, total);
}

TEST_CASE("ChanUtil - Where") {
    RunStats stats;

    auto threes = where<int>([](int n) { return n % 3 == 0; }).spawn();

    stats.spawn([out = std::move(threes.w)]{
        for (int i = 0; i < 20; ++i) {
            out << i;
        }
    });

    int n;
    for (int i = 0; threes.r >> n; i += 3) {
        CHECK_EQ(i, n);
    }
}

TEST_CASE("ChanUtil - WhereAll") {
    RunStats stats;

    // Predicate rejects everything — nothing should pass through.
    auto ch = where<int>([](int) { return false; }).spawn();

    stats.spawn([out = std::move(ch.w)]{
        for (int i = 0; i < 10; ++i) {
            out << i;
        }
    });

    int received = 0;
    stats.spawn([in = std::move(ch.r), &received]{
        int n;
        while (in >> n) {
            ++received;
        }
    });

    ch.release();
    csp::schedule();
    CHECK_EQ(0, received);
}

TEST_CASE("ChanUtil - WhereNone") {
    RunStats stats;

    // Predicate accepts everything — all values pass through.
    auto ch = where<int>([](int) { return true; }).spawn();

    stats.spawn([out = std::move(ch.w)]{
        for (int i = 0; i < 10; ++i) {
            out << i;
        }
    });

    int total = 0;
    stats.spawn([in = std::move(ch.r), &total]{
        for (int n; in >> n;) {
            total += n;
        }
    });

    ch.release();
    csp::schedule();
    CHECK_EQ(45, total);
}

TEST_CASE("ChanUtil - TeeBasic") {
    RunStats stats;

    chan<int> src, dst, side;

    stats.spawn(tee<int>(std::move(side.w)).bind(std::move(src.r), std::move(dst.w)));

    stats.spawn([w = std::move(src.w)]{
        for (int i = 1; i <= 5; ++i) w << i;
    });
    src.release();

    int main_total = 0;
    stats.spawn([r = std::move(dst.r), &main_total]{
        for (int n; r >> n;) main_total += n;
    });
    dst.release();

    int side_total = 0;
    stats.spawn([r = std::move(side.r), &side_total]{
        for (int n; r >> n;) side_total += n;
    });
    side.release();

    csp::schedule();
    CHECK_EQ(15, main_total);
    CHECK_EQ(15, side_total);
}

TEST_CASE("ChanUtil - TeeSideChannelDeath") {
    RunStats stats;

    chan<int> src, dst, side;

    stats.spawn(tee<int>(std::move(side.w)).bind(std::move(src.r), std::move(dst.w)));

    stats.spawn([w = std::move(src.w)]{
        for (int i = 1; i <= 5; ++i) w << i;
    });
    src.release();

    // Side reader reads only 2 values then stops.
    int side_count = 0;
    stats.spawn([r = std::move(side.r), &side_count]{
        int n;
        if (r >> n) ++side_count;
        if (r >> n) ++side_count;
    });
    side.release();

    // Main reader should still receive all 5 values.
    int main_total = 0;
    stats.spawn([r = std::move(dst.r), &main_total]{
        for (int n; r >> n;) main_total += n;
    });
    dst.release();

    csp::schedule();
    CHECK_EQ(2, side_count);
    CHECK_EQ(15, main_total);
}

TEST_CASE("ChanUtil - LatchRepeat") {
    RunStats stats;

    auto lat = latch<int>.spawn();

    stats.spawn([out = std::move(lat.w)]{
        for (int n = 1; n <= 5; ++n) {
            out << n;
        }
    });

    while (csp::internal::run()) { }

    // After writer dies, latch serves the last value repeatedly.
    stats.spawn([in = std::move(lat.r)]{
        CHECK_EQ(5, in.read());
        CHECK_EQ(5, in.read());
        CHECK_EQ(5, in.read());
    });

    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - Zip2") {
    auto r = zip2(count(1, 4).spawn(), count(10, 40, 10).spawn()).spawn();

    auto [a, b] = r.read();
    CHECK_EQ(1, a); CHECK_EQ(10, b);
    std::tie(a, b) = r.read();
    CHECK_EQ(2, a); CHECK_EQ(20, b);
    std::tie(a, b) = r.read();
    CHECK_EQ(3, a); CHECK_EQ(30, b);
    std::pair<int, int> _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Zip2 early termination") {
    // Second stream is shorter — zip2 terminates when it dies.
    auto r = zip2(count(0, 100).spawn(), count(0, 3).spawn()).spawn();

    for (int i = 0; i < 3; ++i) {
        auto [a, b] = r.read();
        CHECK_EQ(i, a);
        CHECK_EQ(i, b);
    }
    std::pair<int, int> _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Zip2f") {
    auto r = zip2f(count(1, 5).spawn(), count(10, 50, 10).spawn(),
                   [](int a, int b) { return a * b; }).spawn();

    CHECK_EQ(10, r.read());
    CHECK_EQ(40, r.read());
    CHECK_EQ(90, r.read());
    CHECK_EQ(160, r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Zip") {
    auto r = zip(count(1, 4).spawn(), count(10, 40, 10).spawn(),
                 count(100, 400, 100).spawn()).spawn();

    auto [a, b, c] = r.read();
    CHECK_EQ(1, a); CHECK_EQ(10, b); CHECK_EQ(100, c);
    std::tie(a, b, c) = r.read();
    CHECK_EQ(2, a); CHECK_EQ(20, b); CHECK_EQ(200, c);
    std::tie(a, b, c) = r.read();
    CHECK_EQ(3, a); CHECK_EQ(30, b); CHECK_EQ(300, c);
    std::tuple<int, int, int> _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Zipf") {
    auto r = zipf<int, int, int>(
                  count(1, 4).spawn(), count(10, 40, 10).spawn(),
                  count(100, 400, 100).spawn(),
                  [](int a, int b, int c) { return a + b + c; }).spawn();

    CHECK_EQ(111, r.read());
    CHECK_EQ(222, r.read());
    CHECK_EQ(333, r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Sinkhole") {
    int latest = 0;
    auto w = sinkhole<int>(latest).spawn();

    for (int i = 1; i <= 10; ++i) {
        w << i;
    }
    CHECK_EQ(10, latest);

    w = {};
    while (csp::internal::run()) { }
}
