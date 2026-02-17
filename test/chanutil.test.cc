#include "testutil.h"

#include <csp/blackhole.h>
#include <csp/chain.h>
#include <csp/count.h>
#include <csp/deaf.h>
#include <csp/enumerate.h>
#include <csp/killswitch.h>
#include <csp/latch.h>
#include <csp/map.h>
#include <csp/mute.h>
#include <csp/sink.h>
#include <csp/tee.h>
#include <csp/where.h>

using namespace csp;

TEST_CASE("ChanUtil - Blackhole") {
    auto w = spawn_blackhole<int>();

    for (int i = 0; i < 1000; ++i) {
        w << i;
    }
}

TEST_CASE("ChanUtil - Chain") {
    std::vector<reader<int>> v1;
    v1.push_back(spawn_count(0, 10));
    v1.push_back(spawn_count(10, 20));
    auto a = spawn_chain<int>(std::move(v1));

    std::vector<reader<int>> v2;
    v2.push_back(spawn_count(20, 30));
    v2.push_back(spawn_count(30, 40));
    auto b = spawn_chain<int>(std::move(v2));

    std::vector<reader<int>> v3;
    v3.push_back(std::move(a));
    v3.push_back(std::move(b));
    auto c = spawn_chain<int>(std::move(v3));

    for (int i = 0, n; c >> n; ++i) {
        CHECK_EQ(i, n);
    }
}

TEST_CASE("ChanUtil - Count") {
    RunStats stats;

    auto e = spawn_count(2, 12345, 7);

    for (int i = 2, n; e >> n; i += 7) {
        CHECK_EQ(i, n);
    }
}

TEST_CASE("ChanUtil - CountCyclic") {
    RunStats stats;

    auto e = spawn_count(2, 15, 7, true);

    for (int i = 0; i < 100; i += 7) {
        CHECK_EQ(2 + i % (15 - 2), e.read());
    }
}

TEST_CASE("ChanUtil - CountForever") {
    RunStats stats;

    auto e = spawn_count_forever(2, 11);

    for (int i = 2, n; i < 10000; i += 11) {
        CHECK(bool(e >> n));
        CHECK_EQ(i, n);
    }
}

TEST_CASE("ChanUtil - Deaf") {
    RunStats stats;

    auto w = spawn_deaf<int>();
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

    reader<int> e = spawn_cycle({2, 3, 5});

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
    auto killswitch = spawn_killswitch<int>(std::move(keepalive_r));

    CHECK(bool(killswitch.w.copy() << 42));
    CHECK_EQ(42, killswitch.r.copy().read());

    keepalive_w = {};
    CHECK_FALSE((killswitch.w.copy() << 21));
    int _;
    CHECK_FALSE((killswitch.r.copy() >> _));
}

TEST_CASE("ChanUtil - Latch") {
    RunStats stats;

    auto latch = spawn_latch<int>();

    stats.spawn([in = latch.r.copy()]{
        CHECK_EQ(1, in.read());
    });

    while (csp::internal::run()) { }

    stats.spawn([out = std::move(latch.w)]{
        for (int n = 1; n <= 5; ++n) {
            out << n;
        }
    });

    while (csp::internal::run()) { }

    stats.spawn([in = std::move(latch.r)]{
        CHECK_EQ(5, in.read());
    });

    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - Map") {
    RunStats stats;

    auto plus_one = spawn_map<int>([](int n) { return n + 1; });

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
    spawn(map(std::move(words_r), std::move(lengths_w), [](auto && s) { return s.length(); }));

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

TEST_CASE("ChanUtil - Mute") {
    RunStats stats;

    auto r = spawn_mute<int>();
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

    auto sink = spawn_sink<int>([&](int n) { total += n; });

    for (int i = 1; i <= 10; ++i) {
        sink << i;
    }

    CHECK_EQ(55, total);
}

TEST_CASE("ChanUtil - Where") {
    RunStats stats;

    auto threes = spawn_where<int>([](int n) { return n % 3 == 0; });

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
    auto ch = spawn_where<int>([](int) { return false; });

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
    auto ch = spawn_where<int>([](int) { return true; });

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

    stats.spawn(tee(std::move(src.r), std::move(dst.w), std::move(side.w)));

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

    stats.spawn(tee(std::move(src.r), std::move(dst.w), std::move(side.w)));

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

    auto latch = spawn_latch<int>();

    stats.spawn([out = std::move(latch.w)]{
        for (int n = 1; n <= 5; ++n) {
            out << n;
        }
    });

    while (csp::internal::run()) { }

    // After writer dies, latch serves the last value repeatedly.
    stats.spawn([in = std::move(latch.r)]{
        CHECK_EQ(5, in.read());
        CHECK_EQ(5, in.read());
        CHECK_EQ(5, in.read());
    });

    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - Sinkhole") {
    int latest = 0;
    auto w = spawn_sinkhole<int>(latest);

    for (int i = 1; i <= 10; ++i) {
        w << i;
    }
    CHECK_EQ(10, latest);

    w = {};
    while (csp::internal::run()) { }
}
