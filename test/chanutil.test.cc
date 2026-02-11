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
    auto w = chan::spawn_blackhole<int>();

    for (int i = 0; i < 1000; ++i) {
        w << i;
    }
}

TEST_CASE("ChanUtil - Chain") {
    auto a = chan::spawn_chain({chan::spawn_count( 0, 10), chan::spawn_count(10, 20)});
    auto b = chan::spawn_chain({chan::spawn_count(20, 30), chan::spawn_count(30, 40)});
    auto c = chan::spawn_chain({a, b});
    for (int i = 0, n; c >> n; ++i) {
        CHECK_EQ(i, n);
    }
}

TEST_CASE("ChanUtil - Count") {
    RunStats stats;

    auto e = chan::spawn_count(2, 12345, 7);

    for (int i = 2, n; e >> n; i += 7) {
        CHECK_EQ(i, n);
    }
}

TEST_CASE("ChanUtil - CountCyclic") {
    RunStats stats;

    auto e = chan::spawn_count(2, 15, 7, true);

    for (int i = 0; i < 100; i += 7) {
        CHECK_EQ(2 + i % (15 - 2), e.read());
    }
}

TEST_CASE("ChanUtil - CountForever") {
    RunStats stats;

    auto e = chan::spawn_count_forever(2, 11);

    for (int i = 2, n; i < 10000; i += 11) {
        CHECK(bool(e >> n));
        CHECK_EQ(i, n);
    }
}

TEST_CASE("ChanUtil - Deaf") {
    RunStats stats;

    auto w = chan::spawn_deaf<int>();
    writer<> give_up;

    stats.spawn([w = std::move(w), give_up = --give_up]{
        CHECK_EQ(-2, prialt(w << 42, ~give_up));
    });

    while (csp_run()) { }
    give_up = {};
    while (csp_run()) { }
}

TEST_CASE("ChanUtil - Enumerate") {
    RunStats stats;

    reader<int> e = chan::spawn_cycle({2, 3, 5});

    int product = 1;

    for (int i = 0; i < 4; ++i) {
        product *= e.read();
    }

    e = {};
    while (csp_run()) { }

    CHECK_EQ(2 * 3 * 5 * 2, product);
}

TEST_CASE("ChanUtil - KillSwitch") {
    RunStats stats;

    writer<> keepalive;
    auto killswitch = chan::spawn_killswitch<int>(--keepalive);


    CHECK(bool(+killswitch << 42));
    CHECK_EQ(42, (-killswitch).read());

    keepalive = {};
    CHECK_FALSE((+killswitch << 21));
    int _;
    CHECK_FALSE((-killswitch >> _));
}

TEST_CASE("ChanUtil - Latch") {
    RunStats stats;

    auto latch = chan::spawn_latch<int>();

    stats.spawn([in = -latch]{
        CHECK_EQ(1, in.read());
    });

    while (csp_run()) { }

    stats.spawn([out = ++latch]{
        for (int n = 1; n <= 5; ++n) {
            out << n;
        }
    });

    while (csp_run()) { }

    stats.spawn([in = --latch]{
        CHECK_EQ(5, in.read());
    });

    while (csp_run()) { }
}

TEST_CASE("ChanUtil - Map") {
    RunStats stats;

    auto plus_one = chan::spawn_map<int>([](int n) { return n + 1; });

    stats.spawn([out = ++plus_one]{
        out << 41;
    });

    stats.spawn([in = --plus_one]{
        CHECK_EQ(42, in.read());
    });

    while (csp_run()) { }
}

TEST_CASE("ChanUtil - MapStrToLen") {
    RunStats stats;

    writer<std::string> words;
    reader<size_t> lengths;;
    spawn(chan::map(--words, ++lengths, [](auto && s) { return s.length(); }));

    stats.spawn([out = std::move(words)]{
        std::string message[] = {"The", "rain", "in", "spain", "falls", "mainly", "on", "the", "plain"};
        for (auto const & word : message) {
            out << word;
        }
    });

    for (size_t i : {3, 4, 2, 5, 5, 6, 2, 3, 5}) {
        CHECK_EQ(i, lengths.read());
    }

    while (csp_run()) { }
}

TEST_CASE("ChanUtil - Mute") {
    RunStats stats;

    auto r = chan::spawn_mute<int>();
    writer<> give_up;

    stats.spawn([r, give_up = --give_up]{
        int n;
        CHECK_GT(0, prialt(r >> n, ~give_up));
    });

    while (csp_run()) { }
    give_up = {};
    while (csp_run()) { }
}

TEST_CASE("ChanUtil - Sink") {
    RunStats stats;

    int total = 0;

    auto sink = chan::spawn_sink<int>([&](int n) { total += n; });

    for (int i = 1; i <= 10; ++i) {
        sink << i;
    }

    CHECK_EQ(55, total);
}

TEST_CASE("ChanUtil - Where") {
    RunStats stats;

    auto threes = chan::spawn_where<int>([](int n) { return n % 3 == 0; });

    stats.spawn([out = ++threes]{
        for (int i = 0; i < 20; ++i) {
            out << i;
        }
    });

    int n;
    for (int i = 0; -threes >> n; i += 3) {
        CHECK_EQ(i, n);
    }
}

TEST_CASE("ChanUtil - WhereAll") {
    RunStats stats;

    // Predicate rejects everything — nothing should pass through.
    auto ch = chan::spawn_where<int>([](int) { return false; });

    stats.spawn([out = ++ch]{
        for (int i = 0; i < 10; ++i) {
            out << i;
        }
    });

    int received = 0;
    stats.spawn([in = --ch, &received]{
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
    auto ch = chan::spawn_where<int>([](int) { return true; });

    stats.spawn([out = ++ch]{
        for (int i = 0; i < 10; ++i) {
            out << i;
        }
    });

    int total = 0;
    stats.spawn([in = --ch, &total]{
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

    channel<int> src, dst, side;

    stats.spawn(chan::tee(-src, +dst, +side));

    stats.spawn([w = +src]{
        for (int i = 1; i <= 5; ++i) w << i;
    });
    src.release();

    int main_total = 0;
    stats.spawn([r = -dst, &main_total]{
        for (int n; r >> n;) main_total += n;
    });
    dst.release();

    int side_total = 0;
    stats.spawn([r = -side, &side_total]{
        for (int n; r >> n;) side_total += n;
    });
    side.release();

    csp::schedule();
    CHECK_EQ(15, main_total);
    CHECK_EQ(15, side_total);
}

TEST_CASE("ChanUtil - TeeSideChannelDeath") {
    RunStats stats;

    channel<int> src, dst, side;

    stats.spawn(chan::tee(-src, +dst, +side));

    stats.spawn([w = +src]{
        for (int i = 1; i <= 5; ++i) w << i;
    });
    src.release();

    // Side reader reads only 2 values then stops.
    int side_count = 0;
    stats.spawn([r = -side, &side_count]{
        int n;
        if (r >> n) ++side_count;
        if (r >> n) ++side_count;
    });
    side.release();

    // Main reader should still receive all 5 values.
    int main_total = 0;
    stats.spawn([r = -dst, &main_total]{
        for (int n; r >> n;) main_total += n;
    });
    dst.release();

    csp::schedule();
    CHECK_EQ(2, side_count);
    CHECK_EQ(15, main_total);
}

TEST_CASE("ChanUtil - LatchRepeat") {
    RunStats stats;

    auto latch = chan::spawn_latch<int>();

    stats.spawn([out = ++latch]{
        for (int n = 1; n <= 5; ++n) {
            out << n;
        }
    });

    while (csp_run()) { }

    // After writer dies, latch serves the last value repeatedly.
    stats.spawn([in = --latch]{
        CHECK_EQ(5, in.read());
        CHECK_EQ(5, in.read());
        CHECK_EQ(5, in.read());
    });

    while (csp_run()) { }
}

TEST_CASE("ChanUtil - Sinkhole") {
    int latest = 0;
    auto w = chan::spawn_sinkhole<int>(latest);

    for (int i = 1; i <= 10; ++i) {
        w << i;
    }
    CHECK_EQ(10, latest);

    w = {};
    while (csp_run()) { }
}
