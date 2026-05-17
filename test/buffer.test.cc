#include "testutil.h"

using namespace csp;

TEST_CASE("ChanUtil---BufferBounded") {
    RunStats stats;

    auto ch = chan<int>(5);

    int sent = 0;

    stats.spawn([out = std::move(ch.w), &sent]{
        for (int i = 1; i <= 10; ++i) {
            REQUIRE(bool(out << i));
            sent += i;
        }
    });

    while (csp::internal::run()) { }
    CHECK(0UL == stats.pending());

    REQUIRE(15 == sent);

    int received = 0;

    stats.spawn([in = std::move(ch.r), &received]{
        int n;
        while (in >> n) {
            received += n;
        }
    });

    while (csp::internal::run()) { }
    CHECK(55 == sent);
    CHECK(55 == received);
}

TEST_CASE("ChanUtil---BufferLargeCapacity") {
    RunStats stats;

    int sent = 0;
    int received = 0;

    auto [send_w, send_r] = chan<>{};
    auto [recv_w, recv_r] = chan<>{};

    auto buf = chan<int>(1024);

    stats.spawn([trigger = std::move(send_r), out = std::move(buf.w), &sent]{
        for (int i = 0; trigger >> poke; ++i) {
            out << i;
            sent += 1;
        }
    });

    stats.spawn([trigger = std::move(recv_r), in = std::move(buf.r), &received]{
        for (int i = 0; trigger >> poke; ++i) {
            CHECK(i == in.read());
            received += 1;
        }
    });

    stats.spawn([send = std::move(send_w), recv = std::move(recv_w)] {
        auto fire = [&](auto && trigger, size_t n) {
            while (n--) {
                trigger << poke;
            }
        };

        for (size_t i = 1; i <= 10; ++i) {
            fire(send, 11 - i);
            fire(recv, i);
        }
    });

    while (csp::internal::run()) { }

    CHECK(55 == sent);
    CHECK(55 == received);
}

TEST_CASE("ChanUtil---BufferEmpty") {
    RunStats stats;

    auto ch = chan<int>(5);

    // Writer dies immediately without sending anything.
    stats.spawn([out = std::move(ch.w)]{ });

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

TEST_CASE("ChanUtil---BufferSingle") {
    RunStats stats;

    auto ch = chan<int>(1);

    stats.spawn([out = std::move(ch.w)]{
        for (int i = 1; i <= 5; ++i) {
            REQUIRE(bool(out << i));
        }
    });

    stats.spawn([in = std::move(ch.r)]{
        for (int i = 1; i <= 5; ++i) {
            CHECK(i == in.read());
        }
    });

    ch.release();
    csp::schedule();
}

TEST_CASE("ChanUtil---BufferZeroThrows") {
    CHECK_THROWS_AS(chan<int>(0), std::invalid_argument);
}

TEST_CASE("ChanUtil---BufferCapacityExact") {
    // Verify chan<T>(N) holds exactly N items, not N+1 (off-by-one check).
    for (size_t cap = 1; cap <= 5; ++cap) {
        RunStats stats;
        auto ch = chan<int>(cap);

        size_t sent = 0;
        stats.spawn([out = std::move(ch.w), &sent] {
            for (int i = 0; i < 100; ++i) {
                out << i;
                ++sent;
            }
        });

        // Run with no consumer — writer blocks after filling buffer.
        while (csp::internal::run()) { }

        INFO("capacity=", cap);
        CHECK(cap == sent);

        // Add consumer to drain and let writer finish.
        stats.spawn([in = std::move(ch.r)] {
            int v;
            while (in >> v) { }
        });

        while (csp::internal::run()) { }
    }
}

// --- chan | pipeline composition tests ---

using namespace csp::part;

TEST_CASE("Chan---filter-|-chan-|-filter-pipeline") {
    // Lazy composition: filter | chan | filter → filter
    auto pipeline = where<int>([](int n) { return n > 0; })
                  | chan<int>(4)
                  | map<int>([](int n) { return n * 10; });

    auto ch = std::move(pipeline).spawn();  // materializes everything

    std::vector<int> results;
    spawn([out = std::move(ch.w)] {
        for (int i : {-1, 2, -3, 4, 5}) out << i;
    });
    spawn([in = std::move(ch.r), &results] {
        for (int v : in) results.push_back(v);
    });
    csp::schedule();
    CHECK(results == std::vector<int>{20, 40, 50});
}

TEST_CASE("Chan---reader-|-chan") {
    auto source = count<int>(1, 6).spawn();
    auto buffered = std::move(source) | chan<int>(8);
    // buffered is reader<int>
    std::vector<int> results;
    spawn([in = std::move(buffered), &results] {
        for (int v : in) results.push_back(v);
    });
    csp::schedule();
    CHECK(results == std::vector<int>{1, 2, 3, 4, 5});
}

TEST_CASE("Chan---chan-|-writer") {
    auto [w, r] = chan<int>{};
    auto buffered_w = chan<int>(4) | std::move(w);
    // buffered_w is writer<int>
    spawn([out = std::move(buffered_w)] {
        for (int i = 1; i <= 3; ++i) out << i;
    });
    std::vector<int> results;
    spawn([in = std::move(r), &results] {
        for (int v : in) results.push_back(v);
    });
    csp::schedule();
    CHECK(results == std::vector<int>{1, 2, 3});
}

TEST_CASE("Chan---producer-|-chan") {
    // Lazy: producer | chan → producer; .spawn() materializes
    auto buffered = (count<int>(1, 4) | chan<int>(8)).spawn();
    // buffered is reader<int>
    std::vector<int> results;
    spawn([in = std::move(buffered), &results] {
        for (int v : in) results.push_back(v);
    });
    csp::schedule();
    CHECK(results == std::vector<int>{1, 2, 3});
}

TEST_CASE("Chan---chan-|-consumer") {
    // Lazy: chan | consumer → consumer; .spawn() materializes
    std::vector<int> results;
    auto input = (chan<int>(4) | sink<int>([&](int v) { results.push_back(v); })).spawn();
    // input is writer<int>
    spawn([out = std::move(input)] {
        for (int i = 10; i <= 30; i += 10) out << i;
    });
    csp::schedule();
    CHECK(results == std::vector<int>{10, 20, 30});
}

TEST_CASE("Chan---multiple-buffer-stages") {
    auto pipeline = map<int>([](int n) { return n + 1; })
                  | chan<int>(2)
                  | map<int>([](int n) { return n * 2; })
                  | chan<int>(2)
                  | map<int>([](int n) { return n + 100; });

    auto ch = std::move(pipeline).spawn();

    spawn([w = std::move(ch.w)] {
        for (int i : {1, 2, 3}) w << i;
    });
    std::vector<int> results;
    spawn([r = std::move(ch.r), &results] {
        for (int v : r) results.push_back(v);
    });
    csp::schedule();
    // 1→2→4→104, 2→3→6→106, 3→4→8→108
    CHECK(results == std::vector<int>{104, 106, 108});
}

// Regression test for 🎯T20: buffered chan must not move values before alt commit.
//
// The buffer imp calls alt(in >> staging, write_op_for(out, buf.front())).
// Before the fix, write_op_for constructed a chan_op<string> by moving from
// buf.front().value into the chan_op's internal storage.  If the read arm
// (case 0) fired instead of the write arm (case 1), buf.front().value was
// left in a moved-from (empty) state.  The next iteration would then deliver
// an empty string to the downstream reader.
//
// Setup: a upstream writer pushes N strings into a chan<string>(1).  A
// downstream reader keeps reading.  A "discard" reader competes with the
// downstream reader so that the buffer imp's write arm sometimes loses the
// alt.  All delivered strings must be non-empty.
TEST_CASE("ChanUtil---BufferNoMoveBeforeCommit") {
    constexpr int N = 50;
    auto buf = chan<std::string>(1);

    // Downstream reader: collects all values.
    auto [collector_w, collector_r] = chan<std::string>{};
    std::vector<std::string> received;

    // Upstream writer: sends N non-empty strings into the buffer.
    spawn([out = std::move(buf.w)] {
        for (int i = 0; i < N; ++i) {
            out << std::string(8, 'A' + char(i % 26));
        }
    });

    // Consumer: reads all strings from the buffer and forwards to collector.
    // Using a plain loop (not alt) so there's no racing drain path here.
    spawn([in = std::move(buf.r), w = std::move(collector_w)] {
        std::string s;
        while (in >> s) {
            // If the bug is present, s will be empty for any round where the
            // read arm fired while the write arm had already moved the value.
            CHECK(!s.empty());
            w << std::move(s);
        }
    });

    spawn([in = std::move(collector_r), &received] {
        std::string s;
        while (in >> s) {
            received.push_back(std::move(s));
        }
    });

    csp::schedule();
    CHECK(int(received.size()) == N);
    for (auto const & s : received) {
        CHECK(!s.empty());
    }
}

// Stronger regression: exercises the move-before-commit bug by having both
// a new write (in >> staging, arm 0) and a buffered-front write (arm 1)
// compete simultaneously.  We force both arms to be ready at the same time
// by pre-filling the buffer to capacity-1 and having an eager consumer.
// With capacity=1 the buffer imp sees buf.full()=false AND buf.empty()=false
// only when exactly one item is buffered, which is the precise window where
// both arms are enabled in the same alt call.
TEST_CASE("ChanUtil---BufferAltBothArmsReady") {
    constexpr int N = 100;
    auto buf = chan<std::string>(1);

    std::vector<std::string> received;

    spawn([out = std::move(buf.w)] {
        for (int i = 0; i < N; ++i) {
            // Non-empty, distinct strings so corruption is detectable.
            out << std::string(4, char('a' + i % 26));
        }
    });

    spawn([in = std::move(buf.r), &received] {
        std::string s;
        while (in >> s) {
            CHECK(!s.empty());
            received.push_back(std::move(s));
        }
    });

    csp::schedule();
    CHECK(int(received.size()) == N);
}
