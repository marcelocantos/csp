#include "testutil.h"

using namespace csp;
using namespace csp::part;

TEST_CASE("ChanUtil - Batch") {
    // 10 elements in batches of 3 → [1,2,3], [4,5,6], [7,8,9], [10]
    auto r = batch<int>(3).spawn(count(1, 11).spawn());

    auto v = r.read();
    CHECK(3 == v.size());
    CHECK(1 == v[0]); CHECK(2 == v[1]); CHECK(3 == v[2]);

    v = r.read();
    CHECK(3 == v.size());
    CHECK(4 == v[0]); CHECK(5 == v[1]); CHECK(6 == v[2]);

    v = r.read();
    CHECK(3 == v.size());
    CHECK(7 == v[0]); CHECK(8 == v[1]); CHECK(9 == v[2]);

    // Partial final batch.
    v = r.read();
    CHECK(1 == v.size());
    CHECK(10 == v[0]);

    std::vector<int> _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Batch exact") {
    // 6 elements in batches of 3 → two full batches, no partial.
    auto r = batch<int>(3).spawn(count(1, 7).spawn());

    auto v = r.read();
    CHECK(3 == v.size());
    CHECK(1 == v[0]);

    v = r.read();
    CHECK(3 == v.size());
    CHECK(4 == v[0]);

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
        CHECK(i == n);
    }
}

TEST_CASE("ChanUtil - Count") {
    RunStats stats;

    auto e = count(2, 12345, 7).spawn();

    for (int i = 2, n; e >> n; i += 7) {
        CHECK(i == n);
    }
}

TEST_CASE("ChanUtil - CountCyclic") {
    RunStats stats;

    auto e = count(2, 15, 7, true).spawn();

    for (int i = 0; i < 100; i += 7) {
        CHECK(2 + i % (15 - 2) == e.read());
    }
}

TEST_CASE("ChanUtil - CountForever") {
    RunStats stats;

    auto e = count_forever(2, 11).spawn();

    for (int i = 2, n; i < 10000; i += 11) {
        CHECK(bool(e >> n));
        CHECK(i == n);
    }
}

TEST_CASE("ChanUtil - Deaf") {
    RunStats stats;

    auto w = deaf<int>.spawn();
    auto [give_up_w, give_up_r] = chan<>{};

    stats.spawn([w = std::move(w), give_up = std::move(give_up_r)]{
        CHECK(~1 == prialt(w << 42, ~give_up));
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

    CHECK(2 * 3 * 5 * 2 == product);
}

TEST_CASE("ChanUtil - KillSwitch") {
    RunStats stats;

    auto [keepalive_w, keepalive_r] = chan<>{};
    auto ks = killswitch<int>(std::move(keepalive_r)).spawn();

    CHECK(bool(ks.w.copy() << 42));
    CHECK(42 == ks.r.copy().read());

    keepalive_w = {};
    CHECK_FALSE((ks.w.copy() << 21));
    int _;
    CHECK_FALSE((ks.r.copy() >> _));
}

TEST_CASE("ChanUtil - Latch") {
    RunStats stats;

    auto lat = latch<int>.spawn();

    stats.spawn([in = lat.r.copy()]{
        CHECK(1 == in.read());
    });

    while (csp::internal::run()) { }

    stats.spawn([out = std::move(lat.w)]{
        for (int n = 1; n <= 5; ++n) {
            out << n;
        }
    });

    while (csp::internal::run()) { }

    stats.spawn([in = std::move(lat.r)]{
        CHECK(5 == in.read());
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
        CHECK(42 == in.read());
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
        CHECK(i == lengths_r.read());
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
    CHECK(15 == got.size());

    std::sort(got.begin(), got.end());
    std::vector<int> expect = {0,1,2,3,4, 10,11,12,13,14, 20,21,22,23,24};
    CHECK(expect == got);
}

TEST_CASE("ChanUtil - Merge single") {
    std::vector<reader<int>> rs;
    rs.push_back(count(1, 4).spawn());
    auto r = merge(std::move(rs)).spawn();

    CHECK(1 == r.read());
    CHECK(2 == r.read());
    CHECK(3 == r.read());
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

    CHECK(1 == r.read());
    CHECK(3 == r.read());
    CHECK(6 == r.read());
    CHECK(10 == r.read());
    CHECK(15 == r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Scan type change") {
    // Accumulate string lengths into an int.
    auto r = scan<std::string, int>(0, [](int acc, std::string s) { return acc + (int)s.size(); })
                 .spawn(enumerate<std::string>({"ab", "cde", "f"}).spawn());

    CHECK(2 == r.read());
    CHECK(5 == r.read());
    CHECK(6 == r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Share single subscriber") {
    RunStats stats;

    std::vector<int> got;
    stats.spawn([&]{
        auto subs = share(count(1, 4).spawn());
        auto r = subs.read();
        subs = {};  // Done subscribing.
        for (int n; r >> n;) got.push_back(n);
    });

    csp::schedule();
    CHECK(std::vector<int>({1, 2, 3}) == got);
}

TEST_CASE("ChanUtil - Share multiple subscribers") {
    RunStats stats;

    stats.spawn([]{
        chan<int> in;
        auto subs = share(std::move(in.r));

        auto a = subs.read();
        auto b = subs.read();
        subs = {};

        // Interleave writes and reads so latches deliver each value.
        in.w << 10;
        CHECK(10 == a.read());
        CHECK(10 == b.read());

        in.w << 20;
        CHECK(20 == a.read());
        CHECK(20 == b.read());

        in.w << 30;
        CHECK(30 == a.read());
        CHECK(30 == b.read());

        in.w = {};
        int _;
        CHECK_FALSE(bool(a >> _));
        CHECK_FALSE(bool(b >> _));
    });

    csp::schedule();
}

TEST_CASE("ChanUtil - Share late subscriber gets current value") {
    RunStats stats;

    int first_val = 0;
    stats.spawn([&]{
        chan<int> in;
        auto subs = share(std::move(in.r));

        auto a = subs.read();

        // Publish two values; subscriber a reads them.
        in.w << 1;
        CHECK(1 == a.read());
        in.w << 2;
        CHECK(2 == a.read());

        // Late subscriber joins — should get current value (2).
        auto b = subs.read();
        first_val = b.read();

        in.w << 3;
        CHECK(3 == a.read());
        CHECK(3 == b.read());

        in.w = {};
        subs = {};
    });

    csp::schedule();
    CHECK(2 == first_val);
}

TEST_CASE("ChanUtil - Share subscriber dropped") {
    RunStats stats;

    stats.spawn([]{
        chan<int> in;
        auto subs = share(std::move(in.r));

        auto a = subs.read();
        auto b = subs.read();
        subs = {};

        // Both get first value.
        in.w << 1;
        CHECK(1 == a.read());
        CHECK(1 == b.read());

        // Drop b.
        b = {};

        // a should still work.
        in.w << 2;
        CHECK(2 == a.read());
        in.w << 3;
        CHECK(3 == a.read());

        in.w = {};
        int _;
        CHECK_FALSE(bool(a >> _));
    });

    csp::schedule();
}

TEST_CASE("ChanUtil - Share source dies") {
    RunStats stats;

    stats.spawn([]{
        auto subs = share(count(1, 3).spawn());
        auto r = subs.read();
        subs = {};

        CHECK(1 == r.read());
        CHECK(2 == r.read());
        int _;
        CHECK_FALSE(bool(r >> _));
    });

    csp::schedule();
}

TEST_CASE("ChanUtil - Metrics basic") {
    auto [data, stats] = metrics(count(1, 6).spawn());

    // Drain all data first.
    std::vector<int> got;
    for (int n; data >> n;) got.push_back(n);
    CHECK(std::vector<int>({1, 2, 3, 4, 5}) == got);

    // Pull final stats.
    auto snap = stats.read();
    CHECK(5 == snap.count);
    CHECK(snap.elapsed.count() > 0);
}

TEST_CASE("ChanUtil - Metrics mid-stream pull") {
    chan<int> in;
    auto [data, stats] = metrics(std::move(in.r));

    csp::spawn([&in]() mutable {
        in.w << 1; in.w << 2; in.w << 3;
        // Pause to let stats be pulled.
        csp::yield();
        in.w << 4; in.w << 5;
        in.w = {};
    });

    // Read 3 values.
    CHECK(1 == data.read());
    CHECK(2 == data.read());
    CHECK(3 == data.read());

    // Pull stats mid-stream.
    auto snap = stats.read();
    CHECK(3 == snap.count);

    // Read remaining.
    CHECK(4 == data.read());
    CHECK(5 == data.read());
    int _;
    CHECK_FALSE(bool(data >> _));
}

TEST_CASE("ChanUtil - Metrics stats reader dropped") {
    auto [data, stats] = metrics(count(1, 4).spawn());

    // Drop the stats reader immediately.
    stats = {};

    // Data should still flow.
    std::vector<int> got;
    for (int n; data >> n;) got.push_back(n);
    CHECK(std::vector<int>({1, 2, 3}) == got);
}

TEST_CASE("ChanUtil - Metrics data reader dropped") {
    auto [data, stats] = metrics(count(1, 100).spawn());

    // Drop both readers — metrics should terminate cleanly.
    data = {};
    stats = {};
    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - Mute") {
    RunStats stats;

    auto r = mute<int>.spawn();
    auto [give_up_w, give_up_r] = chan<>{};

    stats.spawn([r = r.copy(), give_up = std::move(give_up_r)]{
        int n;
        CHECK(0 > prialt(r >> n, ~give_up));
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

    CHECK(55 == total);
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
        CHECK(i == n);
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
    CHECK(0 == received);
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
    CHECK(45 == total);
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
    CHECK(15 == main_total);
    CHECK(15 == side_total);
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
    CHECK(2 == side_count);
    CHECK(15 == main_total);
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
        CHECK(5 == in.read());
        CHECK(5 == in.read());
        CHECK(5 == in.read());
    });

    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - Zip binary") {
    auto r = zip(count(1, 4).spawn(), count(10, 40, 10).spawn()).spawn();

    auto [a, b] = r.read();
    CHECK(1 == a); CHECK(10 == b);
    std::tie(a, b) = r.read();
    CHECK(2 == a); CHECK(20 == b);
    std::tie(a, b) = r.read();
    CHECK(3 == a); CHECK(30 == b);
    std::tuple<int, int> _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Zip early termination") {
    // Second stream is shorter — zip terminates when it dies.
    auto r = zip(count(0, 100).spawn(), count(0, 3).spawn()).spawn();

    for (int i = 0; i < 3; ++i) {
        auto [a, b] = r.read();
        CHECK(i == a);
        CHECK(i == b);
    }
    std::tuple<int, int> _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Zip binary with function") {
    auto r = zip<int, int>(count(1, 5).spawn(), count(10, 50, 10).spawn(),
                           [](int a, int b) { return a * b; }).spawn();

    CHECK(10 == r.read());
    CHECK(40 == r.read());
    CHECK(90 == r.read());
    CHECK(160 == r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Zip") {
    auto r = zip(count(1, 4).spawn(), count(10, 40, 10).spawn(),
                 count(100, 400, 100).spawn()).spawn();

    auto [a, b, c] = r.read();
    CHECK(1 == a); CHECK(10 == b); CHECK(100 == c);
    std::tie(a, b, c) = r.read();
    CHECK(2 == a); CHECK(20 == b); CHECK(200 == c);
    std::tie(a, b, c) = r.read();
    CHECK(3 == a); CHECK(30 == b); CHECK(300 == c);
    std::tuple<int, int, int> _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Zip ternary with function") {
    auto r = zip<int, int, int>(
                  count(1, 4).spawn(), count(10, 40, 10).spawn(),
                  count(100, 400, 100).spawn(),
                  [](int a, int b, int c) { return a + b + c; }).spawn();

    CHECK(111 == r.read());
    CHECK(222 == r.read());
    CHECK(333 == r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Sinkhole") {
    int latest = 0;
    auto w = sinkhole<int>(latest).spawn();

    for (int i = 1; i <= 10; ++i) {
        w << i;
    }
    CHECK(10 == latest);

    w = {};
    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - First") {
    auto r = first<int>(3).spawn(count(1, 11).spawn());

    CHECK(1 == r.read());
    CHECK(2 == r.read());
    CHECK(3 == r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - First short input") {
    auto r = first<int>(5).spawn(count(1, 3).spawn());

    CHECK(1 == r.read());
    CHECK(2 == r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Last") {
    auto r = last<int>(3).spawn(count(1, 11).spawn());

    CHECK(8 == r.read());
    CHECK(9 == r.read());
    CHECK(10 == r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Last short input") {
    auto r = last<int>(5).spawn(count(1, 3).spawn());

    CHECK(1 == r.read());
    CHECK(2 == r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - SkipFirst") {
    auto r = skip_first<int>(3).spawn(count(1, 11).spawn());

    for (int i = 4; i <= 10; ++i) {
        CHECK(i == r.read());
    }
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - SkipFirst short input") {
    auto r = skip_first<int>(5).spawn(count(1, 3).spawn());

    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - SkipLast") {
    auto r = skip_last<int>(3).spawn(count(1, 11).spawn());

    for (int i = 1; i <= 7; ++i) {
        CHECK(i == r.read());
    }
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - SkipLast short input") {
    auto r = skip_last<int>(5).spawn(count(1, 3).spawn());

    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Debounce rapid") {
    using namespace std::chrono_literals;

    // count sends 1–5 instantly. Each replaces pending and restarts timer.
    // Input closes → pending (5) emitted immediately.
    auto r = debounce<int>(50ms).spawn(count(1, 6).spawn());

    CHECK(5 == r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Debounce spaced") {
    using namespace std::chrono_literals;
    RunStats stats;

    auto db = debounce<int>(50ms).spawn();

    stats.spawn([w = std::move(db.w)]{
        w << 1;
        csp::sleep(100ms);
        w << 2;
        csp::sleep(100ms);
        w << 3;
    });

    // Each value has 100ms quiet (> 50ms debounce), so all emitted.
    stats.spawn([r = std::move(db.r)]{
        CHECK(1 == r.read());
        CHECK(2 == r.read());
        CHECK(3 == r.read());
        int _;
        CHECK_FALSE(bool(r >> _));
    });

    csp::schedule();
}

TEST_CASE("ChanUtil - Throttle n=1") {
    using namespace std::chrono_literals;

    // Budget=1, interval=1s. Only first value passes; rest dropped before tick.
    auto r = throttle<int>(tick(1s)).spawn(count(1, 6).spawn());

    CHECK(1 == r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Throttle n=2") {
    using namespace std::chrono_literals;

    // Budget=2, interval=1s. First two pass, rest dropped.
    auto r = throttle<int>(tick(1s), {.n = 2}).spawn(count(1, 6).spawn());

    CHECK(1 == r.read());
    CHECK(2 == r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Throttle budget reset") {
    using namespace std::chrono_literals;
    RunStats stats;

    auto th = throttle<int>(tick(100ms), {.n = 2}).spawn();

    stats.spawn([w = std::move(th.w)]{
        // First burst: 1,2,3.
        w << 1; w << 2; w << 3;
        // Wait for tick to reset budget.
        csp::sleep(150ms);
        // Second burst: 4,5,6.
        w << 4; w << 5; w << 6;
    });

    // First burst: 1,2 pass, 3 dropped.
    // After tick: 4,5 pass, 6 dropped.
    stats.spawn([r = std::move(th.r)]{
        CHECK(1 == r.read());
        CHECK(2 == r.read());
        CHECK(4 == r.read());
        CHECK(5 == r.read());
        int _;
        CHECK_FALSE(bool(r >> _));
    });

    csp::schedule();
}

TEST_CASE("ChanUtil - Sample") {
    RunStats stats;

    auto [trig_w, trig_r] = chan<>{};
    auto r = sample(count(1, 4).spawn(), std::move(trig_r)).spawn();

    stats.spawn([trig_w = std::move(trig_w)]{
        // Give scheduler time to deliver source values before triggering.
        csp::yield();
        trig_w << poke;
        trig_w << poke;
    });

    stats.spawn([r = std::move(r)]{
        // Source 1,2,3 all latched; triggers emit latest (3) twice.
        CHECK(3 == r.read());
        CHECK(3 == r.read());
        int _;
        CHECK_FALSE(bool(r >> _));
    });

    csp::schedule();
}

TEST_CASE("ChanUtil - Delay") {
    using namespace std::chrono_literals;
    RunStats stats;

    auto r = delay<int>(50ms).spawn(count(1, 4).spawn());

    stats.spawn([r = std::move(r)]{
        auto start = std::chrono::steady_clock::now();
        CHECK(1 == r.read());
        CHECK(2 == r.read());
        CHECK(3 == r.read());
        auto elapsed = std::chrono::steady_clock::now() - start;
        CHECK(elapsed >= 45ms);
        int _;
        CHECK_FALSE(bool(r >> _));
    });

    csp::schedule();
}

TEST_CASE("ChanUtil - Timeout no expiry") {
    using namespace std::chrono_literals;

    // count sends 1–5 instantly — well within any timeout.
    auto r = timeout<int>(1s).spawn(count(1, 6).spawn());

    CHECK(1 == r.read());
    CHECK(2 == r.read());
    CHECK(3 == r.read());
    CHECK(4 == r.read());
    CHECK(5 == r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Timeout expiry") {
    using namespace std::chrono_literals;
    RunStats stats;

    auto to = timeout<int>(100ms).spawn();

    stats.spawn([w = std::move(to.w)]{
        w << 1;
        csp::sleep(200ms);
        w << 2;  // Timeout already fired.
    });

    stats.spawn([r = std::move(to.r)]{
        CHECK(1 == r.read());
        int _;
        CHECK_FALSE(bool(r >> _));
    });

    csp::schedule();
}

TEST_CASE("ChanUtil - Distinct adjacent") {
    RunStats stats;

    auto d = distinct<int>().spawn();

    stats.spawn([w = std::move(d.w)]{
        for (int v : {1, 1, 2, 2, 3, 1, 1}) w << v;
    });

    stats.spawn([r = std::move(d.r)]{
        CHECK(1 == r.read());
        CHECK(2 == r.read());
        CHECK(3 == r.read());
        CHECK(1 == r.read());
        int _;
        CHECK_FALSE(bool(r >> _));
    });

    csp::schedule();
}

TEST_CASE("ChanUtil - Distinct all same") {
    RunStats stats;

    auto d = distinct<int>().spawn();

    stats.spawn([w = std::move(d.w)]{
        for (int v : {5, 5, 5, 5}) w << v;
    });

    stats.spawn([r = std::move(d.r)]{
        CHECK(5 == r.read());
        int _;
        CHECK_FALSE(bool(r >> _));
    });

    csp::schedule();
}

TEST_CASE("ChanUtil - Distinct custom comparator") {
    RunStats stats;

    auto ci_eq = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(a[i]) != std::tolower(b[i])) return false;
        return true;
    };
    auto d = distinct<std::string>(ci_eq).spawn();

    stats.spawn([w = std::move(d.w)]{
        for (auto s : {"Foo", "foo", "Bar", "bar", "BAR"})
            w << std::string(s);
    });

    stats.spawn([r = std::move(d.r)]{
        CHECK("Foo" == r.read());
        CHECK("Bar" == r.read());
        std::string _;
        CHECK_FALSE(bool(r >> _));
    });

    csp::schedule();
}

TEST_CASE("ChanUtil - Unique") {
    RunStats stats;

    auto u = unique<int>().spawn();

    stats.spawn([w = std::move(u.w)]{
        for (int v : {1, 2, 3, 2, 1, 4}) w << v;
    });

    stats.spawn([r = std::move(u.r)]{
        CHECK(1 == r.read());
        CHECK(2 == r.read());
        CHECK(3 == r.read());
        CHECK(4 == r.read());
        int _;
        CHECK_FALSE(bool(r >> _));
    });

    csp::schedule();
}

TEST_CASE("ChanUtil - Unique bounded FIFO") {
    RunStats stats;

    // max_remembered=2: remembers last 2 unique values, evicts oldest.
    auto u = unique<int>(2).spawn();

    stats.spawn([w = std::move(u.w)]{
        // 1→emit,set={1}. 2→emit,set={1,2}. 3→evict 1,emit,set={2,3}.
        // 2→in set,suppress. 1→not in set,evict 2,emit,set={3,1}.
        for (int v : {1, 2, 3, 2, 1}) w << v;
    });

    stats.spawn([r = std::move(u.r)]{
        CHECK(1 == r.read());
        CHECK(2 == r.read());
        CHECK(3 == r.read());
        CHECK(1 == r.read());
        int _;
        CHECK_FALSE(bool(r >> _));
    });

    csp::schedule();
}

TEST_CASE("ChanUtil - FlatMap") {
    // Each int n maps to a sub-stream of [n*10, n*10+1, n*10+2].
    auto r = flat_map<int, int>([](int n) {
                 return count(n * 10, n * 10 + 3).spawn();
             })
                 .spawn(count(1, 4).spawn());

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    std::sort(got.begin(), got.end());
    std::vector<int> expect = {10, 11, 12, 20, 21, 22, 30, 31, 32};
    CHECK(expect == got);
}

TEST_CASE("ChanUtil - FlatMap single") {
    auto r = flat_map<int, int>([](int n) {
                 return count(n, n + 2).spawn();
             })
                 .spawn(count(5, 6).spawn());

    CHECK(5 == r.read());
    CHECK(6 == r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - FlatMap empty input") {
    // Input is immediately exhausted — output should close.
    std::vector<reader<int>> empty;
    auto input = merge(std::move(empty)).spawn();
    auto r = flat_map<int, int>([](int n) {
                 return count(n, n + 1).spawn();
             })
                 .spawn(std::move(input));

    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - FlatMap output death") {
    RunStats stats;

    auto r = flat_map<int, int>([](int n) {
                 return count_forever(n).spawn();
             })
                 .spawn(count_forever(0).spawn());

    // Read a few then drop — flat_map should terminate.
    for (int i = 0; i < 10; ++i) r.read();
    r = {};
    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - FlatMap type change") {
    // int -> string sub-streams.
    auto r = flat_map<int, std::string>([](int n) {
                 chan<std::string> ch;
                 csp::spawn([n, w = std::move(ch.w)]() mutable {
                     w << std::to_string(n);
                 });
                 return std::move(ch.r);
             })
                 .spawn(count(1, 4).spawn());

    std::vector<std::string> got;
    for (std::string s; r >> s;) got.push_back(std::move(s));
    std::sort(got.begin(), got.end());
    std::vector<std::string> expect = {"1", "2", "3"};
    CHECK(expect == got);
}

TEST_CASE("ChanUtil - RoundRobin") {
    RunStats stats;

    // Distribute 1..9 across 3 outputs. Must drain concurrently since
    // channels are synchronous and round_robin writes in strict order.
    auto outs = round_robin(count(1, 10).spawn(), 3);
    CHECK(3 == outs.size());

    std::vector<int> g0, g1, g2;
    stats.spawn([r = std::move(outs[0]), &g0]{
        for (int n; r >> n;) g0.push_back(n);
    });
    stats.spawn([r = std::move(outs[1]), &g1]{
        for (int n; r >> n;) g1.push_back(n);
    });
    stats.spawn([r = std::move(outs[2]), &g2]{
        for (int n; r >> n;) g2.push_back(n);
    });
    csp::schedule();

    CHECK(std::vector<int>({1, 4, 7}) == g0);
    CHECK(std::vector<int>({2, 5, 8}) == g1);
    CHECK(std::vector<int>({3, 6, 9}) == g2);
}

TEST_CASE("ChanUtil - RoundRobin single output") {
    auto outs = round_robin(count(1, 4).spawn(), 1);
    CHECK(1 == outs.size());

    CHECK(1 == outs[0].read());
    CHECK(2 == outs[0].read());
    CHECK(3 == outs[0].read());
    int _;
    CHECK_FALSE(bool(outs[0] >> _));
}

TEST_CASE("ChanUtil - RoundRobin output death") {
    RunStats stats;

    auto outs = round_robin(count_forever(0).spawn(), 3);

    // Read concurrently then drop — round_robin should terminate.
    for (auto& r : outs) {
        stats.spawn([r = std::move(r)]{
            for (int i = 0; i < 3; ++i) r.read();
        });
    }
    outs.clear();
    csp::schedule();
    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - Interleave") {
    // Interleave three streams in round-robin order.
    std::vector<reader<int>> rs;
    rs.push_back(count(10, 13).spawn());
    rs.push_back(count(20, 23).spawn());
    rs.push_back(count(30, 33).spawn());
    auto r = interleave(std::move(rs)).spawn();

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    // Strict interleaving: 10,20,30, 11,21,31, 12,22,32.
    std::vector<int> expect = {10, 20, 30, 11, 21, 31, 12, 22, 32};
    CHECK(expect == got);
}

TEST_CASE("ChanUtil - Interleave single") {
    std::vector<reader<int>> rs;
    rs.push_back(count(1, 4).spawn());
    auto r = interleave(std::move(rs)).spawn();

    CHECK(1 == r.read());
    CHECK(2 == r.read());
    CHECK(3 == r.read());
    int _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Interleave uneven") {
    // First stream has 2 elements, second has 4.
    std::vector<reader<int>> rs;
    rs.push_back(count(1, 3).spawn());
    rs.push_back(count(10, 14).spawn());
    auto r = interleave(std::move(rs)).spawn();

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    // 1, 10, 2, 11, <first dies>, 12, 13
    CHECK(std::vector<int>({1, 10, 2, 11, 12, 13}) == got);
}

TEST_CASE("ChanUtil - Interleave output death") {
    RunStats stats;

    std::vector<reader<int>> rs;
    rs.push_back(count_forever(0).spawn());
    rs.push_back(count_forever(0).spawn());
    auto r = interleave(std::move(rs)).spawn();

    // Read a few then drop.
    for (int i = 0; i < 10; ++i) r.read();
    r = {};
    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - Slide fixed") {
    RunStats stats;

    // Window of 3 over 1..6. Both channels must be drained concurrently.
    auto [in, out] = slide<int>(count(1, 7).spawn(), size_t(3));

    std::vector<int> ins, outs;
    stats.spawn([r = std::move(in), &ins]{
        for (int n; r >> n;) ins.push_back(n);
    });
    stats.spawn([r = std::move(out), &outs]{
        for (int n; r >> n;) outs.push_back(n);
    });
    csp::schedule();

    // All 6 elements enter.
    CHECK(std::vector<int>({1, 2, 3, 4, 5, 6}) == ins);
    // Elements 1, 2, 3 expire when 4, 5, 6 arrive.
    CHECK(std::vector<int>({1, 2, 3}) == outs);
}

TEST_CASE("ChanUtil - Slide fixed no slide_in") {
    RunStats stats;

    auto [in, out] = slide<int>(count(1, 7).spawn(), size_t(3), {.slide_in = false});

    std::vector<int> ins, outs;
    stats.spawn([r = std::move(in), &ins]{
        for (int n; r >> n;) ins.push_back(n);
    });
    stats.spawn([r = std::move(out), &outs]{
        for (int n; r >> n;) outs.push_back(n);
    });
    csp::schedule();

    // slide_in=false: no events during growth (elements 1, 2, 3).
    // Events start when element 4 arrives and element 1 expires.
    CHECK(std::vector<int>({4, 5, 6}) == ins);
    CHECK(std::vector<int>({1, 2, 3}) == outs);
}

TEST_CASE("ChanUtil - Slide fixed window larger than input") {
    RunStats stats;

    auto [in, out] = slide<int>(count(1, 4).spawn(), size_t(10));

    std::vector<int> ins, outs;
    stats.spawn([r = std::move(in), &ins]{
        for (int n; r >> n;) ins.push_back(n);
    });
    stats.spawn([r = std::move(out), &outs]{
        for (int n; r >> n;) outs.push_back(n);
    });
    csp::schedule();

    CHECK(std::vector<int>({1, 2, 3}) == ins);
    CHECK(outs.empty());
}

TEST_CASE("ChanUtil - Slide predicate") {
    RunStats stats;

    // Expire when older <= current - 3 (window holds values within range 3).
    auto [in, out] = slide<int>(count(1, 9).spawn(),
        [](const int& older, const int& current) {
            return older <= current - 3;
        });

    std::vector<int> ins, outs;
    stats.spawn([r = std::move(in), &ins]{
        for (int n; r >> n;) ins.push_back(n);
    });
    stats.spawn([r = std::move(out), &outs]{
        for (int n; r >> n;) outs.push_back(n);
    });
    csp::schedule();

    // All elements enter.
    CHECK(std::vector<int>({1, 2, 3, 4, 5, 6, 7, 8}) == ins);
    // 1 expires when 4 arrives, 2 when 5, etc.
    CHECK(std::vector<int>({1, 2, 3, 4, 5}) == outs);
}

TEST_CASE("ChanUtil - Slide output death") {
    RunStats stats;

    auto [in, out] = slide<int>(count_forever(0).spawn(), size_t(3));

    // Read a few from each then drop.
    stats.spawn([r = std::move(in)]{
        for (int i = 0; i < 5; ++i) r.read();
    });
    stats.spawn([r = std::move(out)]{
        for (int i = 0; i < 3; ++i) r.read();
    });
    csp::schedule();
    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - Window") {
    // Window of 3 over 1..6. Emits full vector each time.
    auto r = window<int>(3).spawn(count(1, 7).spawn());

    CHECK(std::vector<int>({1}) == r.read());
    CHECK(std::vector<int>({1, 2}) == r.read());
    CHECK(std::vector<int>({1, 2, 3}) == r.read());
    CHECK(std::vector<int>({2, 3, 4}) == r.read());
    CHECK(std::vector<int>({3, 4, 5}) == r.read());
    CHECK(std::vector<int>({4, 5, 6}) == r.read());
    std::vector<int> _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Window larger than input") {
    auto r = window<int>(10).spawn(count(1, 4).spawn());

    CHECK(std::vector<int>({1}) == r.read());
    CHECK(std::vector<int>({1, 2}) == r.read());
    CHECK(std::vector<int>({1, 2, 3}) == r.read());
    std::vector<int> _;
    CHECK_FALSE(bool(r >> _));
}

TEST_CASE("ChanUtil - Window output death") {
    RunStats stats;

    auto r = window<int>(3).spawn(count_forever(0).spawn());

    for (int i = 0; i < 5; ++i) r.read();
    r = {};
    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - Partition N-way") {
    RunStats stats;

    // Classify 0..8 into 3 buckets by modulo.
    auto outs = partition<int>(count(0, 9).spawn(), 3,
        [](const int& n) -> size_t { return n % 3; });
    CHECK(3 == outs.size());

    std::vector<int> g0, g1, g2;
    stats.spawn([r = std::move(outs[0]), &g0]{
        for (int n; r >> n;) g0.push_back(n);
    });
    stats.spawn([r = std::move(outs[1]), &g1]{
        for (int n; r >> n;) g1.push_back(n);
    });
    stats.spawn([r = std::move(outs[2]), &g2]{
        for (int n; r >> n;) g2.push_back(n);
    });
    csp::schedule();

    CHECK(std::vector<int>({0, 3, 6}) == g0);
    CHECK(std::vector<int>({1, 4, 7}) == g1);
    CHECK(std::vector<int>({2, 5, 8}) == g2);
}

TEST_CASE("ChanUtil - Partition binary") {
    RunStats stats;

    // Split into even/odd.
    auto outs = partition<int>(count(1, 7).spawn(),
        [](const int& n) { return n % 2 != 0; });
    CHECK(2 == outs.size());

    std::vector<int> evens, odds;
    stats.spawn([r = std::move(outs[0]), &evens]{
        for (int n; r >> n;) evens.push_back(n);
    });
    stats.spawn([r = std::move(outs[1]), &odds]{
        for (int n; r >> n;) odds.push_back(n);
    });
    csp::schedule();

    CHECK(std::vector<int>({2, 4, 6}) == evens);
    CHECK(std::vector<int>({1, 3, 5}) == odds);
}

TEST_CASE("ChanUtil - Partition output death") {
    RunStats stats;

    auto outs = partition<int>(count_forever(0).spawn(), 2,
        [](const int& n) -> size_t { return n % 2; });

    // Read a few from each then drop.
    stats.spawn([r = std::move(outs[0])]{
        for (int i = 0; i < 3; ++i) r.read();
    });
    stats.spawn([r = std::move(outs[1])]{
        for (int i = 0; i < 3; ++i) r.read();
    });
    csp::schedule();
    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - Reduce sum") {
    auto result = (count(1, 6) | reduce<int, int>(0,
        [](int acc, int v) { return acc + v; })).spawn().single();
    CHECK(15 == result);  // 1+2+3+4+5
}

TEST_CASE("ChanUtil - Reduce string concat") {
    auto result = (count(1, 4)
        | map<int, std::string>([](int n) { return std::to_string(n); })
        | reduce<std::string, std::string>("",
            [](std::string acc, const std::string& v) { return acc + v; })
    ).spawn().single();
    CHECK("123" == result);
}

TEST_CASE("ChanUtil - Reduce empty") {
    std::vector<reader<int>> empty;
    auto result = (merge(std::move(empty))
        | reduce<int, int>(42,
            [](int acc, int v) { return acc + v; })
    ).spawn().single();
    CHECK(42 == result);  // init returned unchanged
}

TEST_CASE("ChanUtil - Gate") {
    RunStats stats;

    chan<bool> ctrl;
    auto r = gate(count(1, 100).spawn(), std::move(ctrl.r));

    std::vector<int> got;
    stats.spawn([r = std::move(r), &got]{
        for (int n; r >> n;) got.push_back(n);
    });

    // Gate starts open — first 3 values pass.
    stats.spawn([w = std::move(ctrl.w)]{
        csp::yield(); csp::yield(); csp::yield();
        // Close the gate.
        w << false;
        // Re-open.
        csp::yield(); csp::yield();
        w << true;
        csp::yield(); csp::yield(); csp::yield();
        // Close permanently by dropping control.
    });

    csp::schedule();

    // Values should flow while open, pause while closed.
    CHECK(got.size() > 0);
    // First value should be 1 (gate starts open).
    CHECK(1 == got.front());
}

TEST_CASE("ChanUtil - Gate control dies open") {
    RunStats stats;

    chan<bool> ctrl;
    // Control dies immediately — gate stays open (default), forwards all.
    ctrl.w = {};
    auto r = gate(count(1, 4).spawn(), std::move(ctrl.r));

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);

    CHECK(std::vector<int>({1, 2, 3}) == got);
}

TEST_CASE("ChanUtil - Gate output death") {
    RunStats stats;

    chan<bool> ctrl;
    auto r = gate(count_forever(0).spawn(), std::move(ctrl.r));

    for (int i = 0; i < 5; ++i) r.read();
    r = {};
    ctrl.w = {};
    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - GroupBy") {
    RunStats stats;

    auto groups = group_by<int>(
        count(1, 7).spawn(),
        [](int n) { return n % 2; });  // even=0, odd=1

    // Read group announcements and drain sub-streams concurrently.
    std::vector<int> evens, odds;

    stats.spawn([&, groups = std::move(groups)]() mutable {
        std::pair<int, reader<int>> g;
        while (groups >> g) {
            auto& dest = g.first == 0 ? evens : odds;
            csp::spawn([&dest, r = std::move(g.second)]() mutable {
                for (int n; r >> n;) dest.push_back(n);
            });
        }
    });

    csp::schedule();
    CHECK(std::vector<int>({2, 4, 6}) == evens);
    CHECK(std::vector<int>({1, 3, 5}) == odds);
}

TEST_CASE("ChanUtil - GroupBy sub-stream dropped") {
    RunStats stats;

    // Drop the odd group's reader; even group should still work.
    std::vector<int> got;

    stats.spawn([&]{
        auto groups = group_by<int>(
            count(1, 6).spawn(),
            [](int n) { return n % 2; });

        std::pair<int, reader<int>> g;
        while (groups >> g) {
            if (g.first == 0) {
                csp::spawn([&got, r = std::move(g.second)]() mutable {
                    for (int n; r >> n;) got.push_back(n);
                });
            } else {
                g.second = {};  // Drop odd reader — unblocks group_by.
            }
        }
    });

    csp::schedule();
    CHECK(std::vector<int>({2, 4}) == got);
}

TEST_CASE("ChanUtil - GroupBy meta-reader dropped") {
    auto groups = group_by<int>(
        count(1, 100).spawn(),
        [](int n) { return n % 3; });

    // Drop the meta-reader — group_by should terminate.
    groups = {};
    while (csp::internal::run()) { }
}

TEST_CASE("ChanUtil - FirstWins") {
    RunStats stats;

    // Three delayed sources; the first to produce wins.
    chan<int> fast, slow1, slow2;
    stats.spawn([w = std::move(fast.w)]{
        w << 42;
    });
    stats.spawn([w = std::move(slow1.w)]{
        csp::yield(); csp::yield();
        w << 100;
    });
    stats.spawn([w = std::move(slow2.w)]{
        csp::yield(); csp::yield(); csp::yield();
        w << 200;
    });

    std::vector<reader<int>> rs;
    rs.push_back(std::move(fast.r));
    rs.push_back(std::move(slow1.r));
    rs.push_back(std::move(slow2.r));

    int result = -1;
    stats.spawn([rs = std::move(rs), &result]() mutable {
        result = first_wins(std::move(rs));
    });
    csp::schedule();

    CHECK(42 == result);
}

TEST_CASE("ChanUtil - FirstWins with dead readers") {
    RunStats stats;

    // Two dead readers and one live.
    chan<int> live;
    stats.spawn([w = std::move(live.w)]{
        w << 7;
    });

    std::vector<reader<int>> rs;
    // Two immediately-dead readers.
    rs.push_back(merge(std::vector<reader<int>>{}).spawn());
    rs.push_back(merge(std::vector<reader<int>>{}).spawn());
    rs.push_back(std::move(live.r));

    int result = -1;
    stats.spawn([rs = std::move(rs), &result]() mutable {
        result = first_wins(std::move(rs));
    });
    csp::schedule();

    CHECK(7 == result);
}

TEST_CASE("ChanUtil - FirstWins all dead") {
    // All readers dead — should throw.
    std::vector<reader<int>> rs;
    rs.push_back(merge(std::vector<reader<int>>{}).spawn());
    rs.push_back(merge(std::vector<reader<int>>{}).spawn());

    CHECK_THROWS_AS(first_wins(std::move(rs)), std::runtime_error);
}

TEST_CASE("ChanUtil - Join") {
    RunStats stats;

    chan<int> a, b, c;
    stats.spawn([w = std::move(a.w)]{
        w << 1; w << 2;
    });
    stats.spawn([w = std::move(b.w)]{
        w << 10; w << 20; w << 30;
    });
    stats.spawn([w = std::move(c.w)]{
        w << 100;
    });

    std::vector<reader<int>> rs;
    rs.push_back(std::move(a.r));
    rs.push_back(std::move(b.r));
    rs.push_back(std::move(c.r));

    bool done = false;
    stats.spawn([rs = std::move(rs), &done]() mutable {
        join(std::move(rs));
        done = true;
    });
    csp::schedule();

    CHECK(done);
}

TEST_CASE("ChanUtil - Join empty") {
    std::vector<reader<int>> empty;
    join(std::move(empty));  // Should return immediately.
}

TEST_CASE("ChanUtil - Compose filter | filter") {
    // map then where, composed left-to-right.
    auto pipeline = map<int>([](int n) { return n * 2; })
                  | where<int>([](int n) { return n > 4; });
    auto r = pipeline.spawn(count(1, 6).spawn());

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    CHECK(std::vector<int>({6, 8, 10}) == got);
}

TEST_CASE("ChanUtil - Compose producer | filter") {
    // Producer composed with filter.
    auto p = count(1, 6) | map<int>([](int n) { return n * 10; });
    auto r = p.spawn();

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    CHECK(std::vector<int>({10, 20, 30, 40, 50}) == got);
}

TEST_CASE("ChanUtil - Compose three filters") {
    auto pipeline = map<int>([](int n) { return n + 1; })
                  | map<int>([](int n) { return n * 2; })
                  | where<int>([](int n) { return n > 6; });
    auto r = pipeline.spawn(count(1, 6).spawn());

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    // (1+1)*2=4, (2+1)*2=6, (3+1)*2=8, (4+1)*2=10, (5+1)*2=12
    CHECK(std::vector<int>({8, 10, 12}) == got);
}

TEST_CASE("ChanUtil - Compose producer | filter chain") {
    auto p = count(1, 4)
           | map<int>([](int n) { return n * 3; })
           | where<int>([](int n) { return n > 3; });
    auto r = p.spawn();

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    CHECK(std::vector<int>({6, 9}) == got);
}

TEST_CASE("ChanUtil - Compose filter | consumer") {
    std::vector<int> got;
    auto w = (map<int>([](int n) { return n * 2; })
            | sink<int>([&got](int n) { got.push_back(n); }))
              .spawn();

    w << 1; w << 2; w << 3;
    w = {};
    while (csp::internal::run()) { }

    CHECK(std::vector<int>({2, 4, 6}) == got);
}

TEST_CASE("ChanUtil - Compose producer | consumer") {
    std::vector<int> got;
    auto run_it = count(1, 4)
                | sink<int>([&got](int n) { got.push_back(n); });
    csp::spawn(std::move(run_it));
    while (csp::internal::run()) { }

    CHECK(std::vector<int>({1, 2, 3}) == got);
}

TEST_CASE("ChanUtil - Compose reader | filter") {
    auto r = count(1, 6).spawn()
           | map<int>([](int n) { return n * 2; });

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    CHECK(std::vector<int>({2, 4, 6, 8, 10}) == got);
}

TEST_CASE("ChanUtil - Compose reader | consumer") {
    std::vector<int> got;
    auto run_it = count(1, 4).spawn()
                | sink<int>([&got](int n) { got.push_back(n); });
    csp::spawn(std::move(run_it));
    while (csp::internal::run()) { }

    CHECK(std::vector<int>({1, 2, 3}) == got);
}

TEST_CASE("ChanUtil - Compose filter | writer") {
    chan<int> ch;
    auto w = map<int>([](int n) { return n * 2; })
           | std::move(ch.w);

    std::vector<int> got;
    csp::spawn([&got, r = std::move(ch.r)]() mutable {
        for (int n; r >> n;) got.push_back(n);
    });
    w << 1; w << 2; w << 3;
    w = {};
    while (csp::internal::run()) { }

    CHECK(std::vector<int>({2, 4, 6}) == got);
}

TEST_CASE("ChanUtil - Compose producer | writer") {
    chan<int> ch;
    auto run_it = count(1, 4) | std::move(ch.w);

    std::vector<int> got;
    csp::spawn([&got, r = std::move(ch.r)]() mutable {
        for (int n; r >> n;) got.push_back(n);
    });
    csp::spawn(std::move(run_it));
    while (csp::internal::run()) { }

    CHECK(std::vector<int>({1, 2, 3}) == got);
}

TEST_CASE("ChanUtil - Compose reader | filter | filter") {
    auto r = count(1, 6).spawn()
           | map<int>([](int n) { return n + 10; })
           | where<int>([](int n) { return n > 13; });

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    CHECK(std::vector<int>({14, 15}) == got);
}

// --- take_while / skip_while ---

TEST_CASE("ChanUtil - TakeWhile") {
    auto r = take_while<int>([](int n) { return n < 4; })
                 .spawn(count(1, 8).spawn());

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    CHECK(std::vector<int>({1, 2, 3}) == got);
}

TEST_CASE("ChanUtil - TakeWhile all pass") {
    auto r = take_while<int>([](int n) { return n < 100; })
                 .spawn(count(1, 4).spawn());

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    CHECK(std::vector<int>({1, 2, 3}) == got);
}

TEST_CASE("ChanUtil - TakeWhile none pass") {
    auto r = take_while<int>([](int) { return false; })
                 .spawn(count(1, 4).spawn());

    int n;
    CHECK_FALSE(bool(r >> n));
}

TEST_CASE("ChanUtil - SkipWhile") {
    auto r = skip_while<int>([](int n) { return n < 4; })
                 .spawn(count(1, 8).spawn());

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    CHECK(std::vector<int>({4, 5, 6, 7}) == got);
}

TEST_CASE("ChanUtil - SkipWhile all skipped") {
    auto r = skip_while<int>([](int) { return true; })
                 .spawn(count(1, 4).spawn());

    int n;
    CHECK_FALSE(bool(r >> n));
}

TEST_CASE("ChanUtil - SkipWhile none skipped") {
    auto r = skip_while<int>([](int) { return false; })
                 .spawn(count(1, 4).spawn());

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    CHECK(std::vector<int>({1, 2, 3}) == got);
}

// --- pairwise ---

TEST_CASE("ChanUtil - Pairwise") {
    auto r = pairwise<int>.spawn(count(1, 6).spawn());

    CHECK(std::make_pair(1, 2) == r.read());
    CHECK(std::make_pair(2, 3) == r.read());
    CHECK(std::make_pair(3, 4) == r.read());
    CHECK(std::make_pair(4, 5) == r.read());
    std::pair<int, int> p;
    CHECK_FALSE(bool(r >> p));
}

TEST_CASE("ChanUtil - Pairwise single element") {
    auto r = pairwise<int>.spawn(count(1, 2).spawn());

    std::pair<int, int> p;
    CHECK_FALSE(bool(r >> p));
}

TEST_CASE("ChanUtil - Pairwise empty") {
    auto r = pairwise<int>.spawn(
        merge(std::vector<reader<int>>{}).spawn());

    std::pair<int, int> p;
    CHECK_FALSE(bool(r >> p));
}

// --- nwise ---

TEST_CASE("ChanUtil - Nwise 3") {
    auto r = nwise<3, int>().spawn(count(1, 7).spawn());

    CHECK(std::make_tuple(1, 2, 3) == r.read());
    CHECK(std::make_tuple(2, 3, 4) == r.read());
    CHECK(std::make_tuple(3, 4, 5) == r.read());
    CHECK(std::make_tuple(4, 5, 6) == r.read());
    std::tuple<int, int, int> t;
    CHECK_FALSE(bool(r >> t));
}

TEST_CASE("ChanUtil - Nwise 2 matches pairwise") {
    auto r = nwise<2, int>().spawn(count(1, 5).spawn());

    CHECK(std::make_tuple(1, 2) == r.read());
    CHECK(std::make_tuple(2, 3) == r.read());
    CHECK(std::make_tuple(3, 4) == r.read());
    std::tuple<int, int> t;
    CHECK_FALSE(bool(r >> t));
}

TEST_CASE("ChanUtil - Nwise too few elements") {
    auto r = nwise<5, int>().spawn(count(1, 4).spawn());

    std::tuple<int, int, int, int, int> t;
    CHECK_FALSE(bool(r >> t));
}

// --- flatten ---

TEST_CASE("ChanUtil - Flatten") {
    auto r = flatten<int>.spawn(
        batch<int>(3).spawn(count(1, 8).spawn()));

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    CHECK(std::vector<int>({1, 2, 3, 4, 5, 6, 7}) == got);
}

TEST_CASE("ChanUtil - Flatten empty vectors") {
    auto [w, rd] = chan<std::vector<int>>{};
    auto r = flatten<int>.spawn(std::move(rd));

    csp::spawn([w = std::move(w)] {
        w << std::vector<int>{};
        w << std::vector<int>{1, 2};
        w << std::vector<int>{};
        w << std::vector<int>{3};
    });

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    CHECK(std::vector<int>({1, 2, 3}) == got);
}

// --- unzip ---

TEST_CASE("ChanUtil - Unzip binary") {
    auto src = zip(count(1, 4).spawn(), count(10, 40, 10).spawn()).spawn();
    auto [ra, rb] = unzip(std::move(src));

    std::vector<int> as, bs;
    csp::spawn([ra = std::move(ra), &as]{
        for (int n; ra >> n;) as.push_back(n);
    });
    csp::spawn([rb = std::move(rb), &bs]{
        for (int n; rb >> n;) bs.push_back(n);
    });
    while (csp::internal::run()) { }

    CHECK(std::vector<int>({1, 2, 3}) == as);
    CHECK(std::vector<int>({10, 20, 30}) == bs);
}

TEST_CASE("ChanUtil - Unzip with function (binary)") {
    // Decompose int into quotient and remainder.
    auto [rq, rr] = unzip(count(0, 5).spawn(),
        [](int n) { return std::make_pair(n / 2, n % 2); });

    std::vector<int> qs, rs;
    csp::spawn([rq = std::move(rq), &qs]{
        for (int n; rq >> n;) qs.push_back(n);
    });
    csp::spawn([rr = std::move(rr), &rs]{
        for (int n; rr >> n;) rs.push_back(n);
    });
    while (csp::internal::run()) { }

    CHECK(std::vector<int>({0, 0, 1, 1, 2}) == qs);
    CHECK(std::vector<int>({0, 1, 0, 1, 0}) == rs);
}

TEST_CASE("ChanUtil - Unzip tuple") {
    auto src = zip(count(1, 4).spawn(),
                   count(10, 40, 10).spawn(),
                   count(100, 400, 100).spawn()).spawn();
    auto [ra, rb, rc] = unzip(std::move(src));

    std::vector<int> as, bs, cs;
    csp::spawn([ra = std::move(ra), &as]{
        for (int n; ra >> n;) as.push_back(n);
    });
    csp::spawn([rb = std::move(rb), &bs]{
        for (int n; rb >> n;) bs.push_back(n);
    });
    csp::spawn([rc = std::move(rc), &cs]{
        for (int n; rc >> n;) cs.push_back(n);
    });
    while (csp::internal::run()) { }

    CHECK(std::vector<int>({1, 2, 3}) == as);
    CHECK(std::vector<int>({10, 20, 30}) == bs);
    CHECK(std::vector<int>({100, 200, 300}) == cs);
}

TEST_CASE("ChanUtil - Unzip with function (ternary)") {
    // Decompose int into three fields via function.
    auto readers = unzip(count(0, 4).spawn(),
        [](int n) { return std::make_tuple(n, n * 10, n * 100); });
    auto& [ra, rb, rc] = readers;

    std::vector<int> as, bs, cs;
    csp::spawn([ra = std::move(ra), &as]{
        for (int n; ra >> n;) as.push_back(n);
    });
    csp::spawn([rb = std::move(rb), &bs]{
        for (int n; rb >> n;) bs.push_back(n);
    });
    csp::spawn([rc = std::move(rc), &cs]{
        for (int n; rc >> n;) cs.push_back(n);
    });
    while (csp::internal::run()) { }

    CHECK(std::vector<int>({0, 1, 2, 3}) == as);
    CHECK(std::vector<int>({0, 10, 20, 30}) == bs);
    CHECK(std::vector<int>({0, 100, 200, 300}) == cs);
}

// --- default_if_empty ---

TEST_CASE("ChanUtil - DefaultIfEmpty with values") {
    auto r = default_if_empty<int>(99)
                 .spawn(count(1, 4).spawn());

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    CHECK(std::vector<int>({1, 2, 3}) == got);
}

TEST_CASE("ChanUtil - DefaultIfEmpty empty input") {
    auto r = default_if_empty<int>(99)
                 .spawn(merge(std::vector<reader<int>>{}).spawn());

    CHECK(99 == r.read());
    int n;
    CHECK_FALSE(bool(r >> n));
}

// --- stride ---

TEST_CASE("ChanUtil - Stride 3") {
    auto r = stride<int>(3).spawn(count(1, 11).spawn());

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    CHECK(std::vector<int>({1, 4, 7, 10}) == got);
}

TEST_CASE("ChanUtil - Stride 1 is identity") {
    auto r = stride<int>(1).spawn(count(1, 5).spawn());

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    CHECK(std::vector<int>({1, 2, 3, 4}) == got);
}

TEST_CASE("ChanUtil - Stride 2") {
    auto r = stride<int>(2).spawn(count(1, 8).spawn());

    std::vector<int> got;
    for (int n; r >> n;) got.push_back(n);
    CHECK(std::vector<int>({1, 3, 5, 7}) == got);
}
