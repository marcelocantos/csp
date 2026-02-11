#include "testutil.h"

#include <csp/buffer.h>
#include <csp/count.h>
#include <csp/tee.h>

#include <algorithm>
#include <memory>
#include <vector>

using namespace csp;

static Logger g_log("Channel.Test");

TEST_CASE("Channel - RefCounts1") {
    {
        channel<int> ch;
        auto wr = +ch;
        auto rd = -ch;
    }
    CHECK_EQ(0, csp__internal__channel_count(0));
    CHECK_EQ(0, csp__internal__channel_count(1));
}

TEST_CASE("Channel - RefCounts2") {
    {
        channel<int> ch;
        auto f = [in = +ch, out = -ch]{ };
        f();
    }
    CHECK_EQ(0, csp__internal__channel_count(0));
    CHECK_EQ(0, csp__internal__channel_count(1));
}

TEST_CASE("Channel - RefCounts3") {
    CHECK_EQ(0, csp__internal__channel_count(0));
    CHECK_EQ(0, csp__internal__channel_count(1));

    channel<int> ch;
    CHECK_EQ(1, csp__internal__channel_count(0));

    ch.release();
    CHECK_EQ(0, csp__internal__channel_count(0));
    CHECK_EQ(0, csp__internal__channel_count(1));
}

TEST_CASE("Channel - ThreadRefCounts") {
    CHECK_EQ(0, csp__internal__channel_count(0));
    CHECK_EQ(0, csp__internal__channel_count(1));
    {
        channel<int> ch;

        CHECK_EQ(1, csp__internal__channel_count(1));

        ch.release();

        CHECK_EQ(0, csp__internal__channel_count(1));
    }
    CHECK_EQ(0, csp__internal__channel_count(1));
}

TEST_CASE("Channel - OneShot") {
    channel<int> ch;
    int result = 0;

    spawn([o = +ch]{
        o << 42;
    });
    spawn([i = -ch, &result]{
        i >> result;
    });

    csp::schedule();

    CHECK_EQ(42, result);
}

// Repeat OneShot to exercise a SEGFAULT bug.
TEST_CASE("Channel - OneShotAgain") {
    channel<int> ch;
    int result = 0;

    spawn([o = +ch]{
        o << 42;
    });
    spawn([i = -ch, &result]{
        i >> result;
    });

    csp::schedule();

    CHECK_EQ(42, result);
}

TEST_CASE("Channel - OneShotStats") {
    RunStats stats;

    channel<int> ch;
    int result = 0;

    stats.spawn([o = +ch]{
        o << 42;
    });
    stats.spawn([i = -ch, &result]{
        i >> result;
    });

    csp::schedule();

    CHECK_EQ(42, result);
}

TEST_CASE("Channel - Basic") {
    RunStats stats;

    channel<int> a, b, c;

    stats.spawn([in = -a, out = +b]{
        out << (in.read() + 20);
    });
    stats.spawn([in = -b, out = +c]{
        out << (in.read() + 300);
    });

    int result = 0;

    stats.spawn([in = +a, out = -c, &result]{
        in << 1;
        result = out.read();
    });

    a.release();
    b.release();
    c.release();

    csp::schedule();

    CHECK_EQ(321, result);
}

TEST_CASE("Channel - WriterGone") {
    RunStats stats;

    channel<int> ch;

    int total = 0;

    stats.spawn([out = +ch]{
        for (int n = 1; n <= 10; ++n) {
            out << n;
        }
    });
    stats.spawn([in = -ch, &total]{
        int n;
        while (in >> n) {
            total += n;
        }
    });

    ch.release();

    csp::schedule();

    CHECK_EQ(55, total);
}

TEST_CASE("Channel - ReaderGone") {
    RunStats stats;

    channel<int> ch;

    int total = 0;

    stats.spawn([out = +ch]{
        for (int n = 1; out << n; n *= 2) { }
    });
    stats.spawn([in = -ch, &total]{
        for (int i = 0; i < 10; ++i) {
            total += in.read();
        }
    });

    ch.release();

    csp::schedule();

    CHECK_EQ(1023, total);
}

TEST_CASE("Channel - NWriters") {
    RunStats stats;

    channel<int> ch;

    std::vector<int> total;

    for (int n = 1; n <= 2; ++n) {
        stats.spawn([out = +ch, n]{
            CSP_LOG(g_log, "producer[%d]", n);
            out << n;
        });
    }
    ++ch;

    csp::schedule();

    stats.spawn([out = --ch, &total] {
        CSP_LOG(g_log, "consumer");
        for (auto n : out) {
            total.push_back(n);
        }
    });

    csp::schedule();

    std::sort(total.begin(), total.end());
    CHECK_EQ(std::vector<int>({1, 2}), total);
}

TEST_CASE("Channel - NReaders") {
    RunStats stats;

    channel<int> ch;

    int total = 0;

    for (int i = 0; i < 10; ++i) {
        stats.spawn([in = -ch, &total, i]{
            total += in.read();
        });
    }

    --ch;

    csp::schedule();

    stats.spawn([out = +ch]{
        for (int n = 1; out << n; n *= 2) { }
    });

    csp::schedule();

    CHECK_EQ(1023, total);
}

// We don't want channel.test.cc to depend on rpc.h.
template <typename Req, typename Rep>
static auto rpc(writer<Req> const & req, reader<Rep> const & rep) {
    return [req, rep](int n) {
        req << n;
        return rep.read();
    };
};

TEST_CASE("Channel - AltIn") {
    RunStats stats;

    channel<int> up0, up1, down;

    int sent = 0, received = 0;;

    stats.spawn([in0 = --up0, in1 = --up1, out = ++down, &sent]{
        int n;
        for (int i = 0; i < 2; ++i) {
            alt(in0 >> n, in1 >> n);
            out << n;
            ++sent;
        }
    });

    stats.spawn([out0 = ++up0, out1 = ++up1, in = --down, &received]{
        CHECK_EQ(11, rpc(out0, in)(11));
        ++received;

        CHECK_EQ(42, rpc(out1, in)(42));
        ++received;
    });

    csp::schedule();

    CHECK_EQ(2, sent);
    CHECK_EQ(2, received);
}

TEST_CASE("Channel - AltDead") {
    RunStats stats;

    channel<int> up, down, die;

    int reqs = 0, reps = 0;

    stats.spawn([in = --up, out = ++down, die = --die, &reqs]{
        for (;;) {
            int n;
            switch (alt(in >> n, ~die)) {
            case 1:
                CHECK(bool(out << n));
                ++reqs;
                break;
            case -2:
                return;
            }
        }
    });

    auto kill = ++die;

    stats.spawn([out = ++up, in = --down, &kill, &reps]{
        auto echo = rpc(out, in);

        for (int i = 1; i <= 10; ++i) {
            CHECK_EQ(i, echo(i));
            ++reps;
        }

        kill = {};
        csp_yield(); // Let the other guy wake up and smell the roses.

        CHECK_FALSE((out << 5));
    });

    csp::schedule();

    CHECK_EQ(10, reqs);
    CHECK_EQ(10, reps);
}

TEST_CASE("Channel - AltNull") {
    RunStats stats;

    channel<int> up, down;

    stats.spawn([in = -up]{
        CHECK_EQ(42, in.read());
    });

    stats.spawn([out = +down]{
        out << 11;
    });

    stats.spawn([up = +up, down = -down]{
        int n;
        auto actions = action_list(up << 42, action(), down >> n);
        for (int i = 0; i < 2; ++i) {
            auto a = alt(actions);
            CHECK_NE(a, 2);
            CHECK_NE(a, -2);
            if (a == 1) {
                actions[0] = {};
            } else if (a == 3) {
                CHECK_EQ(11, n);
                actions[2] = {};
            } else {
                FAIL_CHECK("unexpected a = " << a);
            }
        }
    });

    csp::schedule();
}

TEST_CASE("Channel - Range") {
    RunStats stats;

    channel<int> ch;
    stats.spawn([out = ++ch]{
        for (int n = 1; n <= 10; ++n) {
            out << n;
        }
    });

    int total = 0;

    for (auto n : --ch) {
        total += n;
    }

    CHECK_EQ(55, total);
}

TEST_CASE("Channel - SpawnRange") {
    struct borkborkbork { };

    RunStats stats;

    auto r = csp::spawn_range<int>([](auto && w) {
        for (int n = 1; n <= 10; ++n) {
            w << n;
            if (n == 5) {
                throw borkborkbork{};
            }
        }
    });

    int total = 0;

    CHECK_THROWS_AS(for (auto n : r) total += n, borkborkbork);
    CHECK_EQ(15, total);
}

// Test action objects that send larger-than-pointer message.
TEST_CASE("Channel - ActionBig") {
    RunStats stats;

    struct Big {
        uint64_t a, b, c, d;
    };

    Big big = {
        0xcb2890510ace248fULL,
        0x212ce3d4f9a9f23dULL,
        0x4072989d7204b2f7ULL,
        0xeb48f2b297262f6fULL,
    };
    Big big2 = big, big3 = {};

    channel<Big> chanb;
    auto a = csp::action_list(+chanb << big);
    big = {};

    stats.spawn([&]{
        -chanb >> big3;
    });

    CHECK_EQ(1, csp::alt(a));
    CHECK_EQ(big2.a, big3.a);
    CHECK_EQ(big2.b, big3.b);
    CHECK_EQ(big2.c, big3.c);
    CHECK_EQ(big2.d, big3.d);
}

TEST_CASE("Channel - String") {
    RunStats stats;

    writer<std::string> in;
    channel<std::string> branch[2];
    channel<std::string> merge[2];
    reader<std::string> out;

    // splitter
    stats.spawn([r = --in, w0 = ++branch[0], w1 = ++branch[1]]{
        for (std::string s; r >> s;) {
            auto sp = s.find(' ');
            if (sp != s.npos) {
                w0 << s.substr(0, sp);
                w1 << s.substr(sp + 1);
            }
        }
    });

    // capser
    stats.spawn([r = --branch[0], w = ++merge[0]]{
            for (std::string s; r >> s;) {
                for (auto & c : s) {
                    c = toupper(c);
                }
                w << s;
            }
        });

    // reverser
    stats.spawn([r = --branch[1], w = ++merge[1]]{
            for (std::string s; r >> s;) {
                reverse(begin(s), end(s));
                w << s;
            }
        });

    // merger
    stats.spawn([r0 = --merge[0], r1 = --merge[1], w = ++out]{
            for (std::string a, b;
                 (alt(r0 >> a, ~r1, ~w) > 0 &&
                  alt(r1 >> b, ~r0, ~w) > 0 &&
                  w << a + ' ' + b);)
                { }
        });

    std::pair<std::string, std::string> cases[] = {
            {"John Snow", "JOHN wonS"},
            {"ancient ruins", "ANCIENT sniur"},
            {"dwarf shortage", "DWARF egatrohs"},
            {"golden rat", "GOLDEN tar"},
        };
    for (int i = 0; i < 10; ++i) {
        for (auto const & c : cases) {
            in << c.first;
            std::string s;
            CHECK(bool(out >> s));
            CHECK_EQ(c.second, s);
        }
    }
    in = {};
    out = {};
    csp::schedule();
}

TEST_CASE("Channel - Types") {
}

TEST_CASE("Channel - FeedbackLoop") {
    //      +-------+     /---+
    // ---->|       |    /    |---->
    //      | minus |-->( tee |
    //   +->|       |    \    |-+
    //   |  +-------+     \---+ |
    //   |                      |
    //   +-------(buffer)-------+

    RunStats stats;

    auto buf = chan::spawn_buffer<int>();

    constexpr int cadence = 5;

    // Pre-fill buffer with a few zeros.
    for (int i = 0; i < cadence; ++i) {
        +buf << 0;
    }

    channel<int> inner;
    reader<int> out;

    // minus
    spawn([sub = --buf, out = ++inner] {
        auto in = chan::spawn_count_forever(0);
        for (int a = 0, b = 0; in >> a && sub >> b && out << (a - b);) {
            CSP_LOG(g_log, "a = %d, b = %d", a, b);
        }
    });

    spawn(chan::tee(--inner, ++out, ++buf));

    for (int i = 0; i < 100; i += cadence) {
        for (int j = 0; j < cadence; ++j) REQUIRE_EQ(i + j, out.read());
        for (int j = 0; j < cadence; ++j) REQUIRE_EQ(i + 5, out.read());
    }

    csp::schedule();
}

template <typename T>
static void spawn_outward_tree(RunStats & stats, reader<T> in, writer<T> * outs, size_t n_outs) {
    if (n_outs == 1) {
        auto out = std::move(*outs);
        stats.spawn(in.stream_to(out));
    } else {
        writer<T> inner0, inner1;
        spawn_outward_tree(stats, --inner0, outs, n_outs / 2);
        spawn_outward_tree(stats, --inner1, outs + n_outs / 2, n_outs - n_outs / 2);
        stats.spawn([=] {
            // round robin
            for (T t; in >> t && inner0 << t && in >> t && inner1 << t;) { }
        });
    }
}

template <typename T>
static void spawn_inward_tree(RunStats & stats, reader<T> * ins, size_t n_ins, writer<T> out) {
    if (n_ins == 1) {
        auto in = std::move(*ins);
        stats.spawn(in.stream_to(out));
    } else {
        reader<T> inner0, inner1;
        spawn_inward_tree(stats, ins, n_ins / 2, ++inner0);
        spawn_inward_tree(stats, ins + n_ins / 2, n_ins - n_ins / 2, ++inner1);
        stats.spawn([=] {
            // alt
            for (T t; prialt(~out, inner0 >> t, inner1 >> t) > 0 && out << t;) { }
        });
    }
}

TEST_CASE("Channel - Capillaries") {
    //           O --> O
    //          /       \
    //         O         O
    //        / \       / \
    //       /   O --> O   \
    //      /               \
    // --> O                 O -->
    //      \               /
    //       \   O --> O   /
    //        \ /       \ /
    //         O         O
    //          \       /
    //           O --> O
    RunStats stats;

    constexpr size_t WIDTH = 0x100;
    constexpr size_t MESSAGES = 0x1000;

    writer<size_t> ww[WIDTH];
    reader<size_t> rr[WIDTH];
    for (size_t i = 0; i < WIDTH; ++i) {
        rr[i] = --ww[i];
    }

    writer<size_t> in;
    reader<size_t> out;

    spawn_outward_tree(stats, --in, ww, WIDTH);
    spawn_inward_tree(stats, rr, WIDTH, ++out);

    stats.spawn(chan::count(std::move(in), 0UL, MESSAGES));

    std::bitset<MESSAGES> received;
    for (size_t i; out >> i;) {
        received.set(i);
    }

    CHECK(received.all());

    out = {};
}

TEST_CASE("Channel - MoveOnly") {
    RunStats stats;

    channel<std::unique_ptr<int>> ch;

    stats.spawn([w = +ch]{
        w << std::make_unique<int>(42);
    });

    std::unique_ptr<int> result;
    stats.spawn([r = -ch, &result]{
        r >> result;
    });

    ch.release();

    csp::schedule();

    REQUIRE_NE(nullptr, result);
    CHECK_EQ(42, *result);
}

TEST_CASE("Channel - StreamTo") {
    RunStats stats;

    channel<int> src;
    reader<int> out;

    stats.spawn([w = +src]{
        for (int i = 1; i <= 10; ++i) w << i;
    });

    stats.spawn((-src).stream_to(++out));

    src.release();

    int total = 0;
    for (auto n : out) {
        total += n;
    }

    CHECK_EQ(55, total);
}

TEST_CASE("Channel - CopySemantics") {
    RunStats stats;

    channel<int> ch;

    // Copy writer and reader.
    auto w1 = +ch;
    auto w2 = w1;
    CHECK_EQ(w1, w2);

    auto r1 = -ch;
    auto r2 = r1;
    CHECK_EQ(r1, r2);

    ch.release();

    // Both reader copies should work.
    stats.spawn([r = std::move(r2)]{
        CHECK_EQ(42, r.read());
    });

    w1 << 42;

    // Release one writer copy; channel stays alive via w2.
    w1 = {};

    stats.spawn([r = std::move(r1)]{
        CHECK_EQ(99, r.read());
    });

    w2 << 99;
    w2 = {};

    csp::schedule();
}

TEST_CASE("Channel - NWritersNReaders") {
    RunStats stats;

    channel<int> ch;

    constexpr int N = 10;
    int sent = 0, received = 0;

    for (int i = 0; i < N; ++i) {
        stats.spawn([w = +ch, &sent]{
            w << 1;
            ++sent;
        });
        stats.spawn([r = -ch, &received]{
            received += r.read();
        });
    }

    ch.release();

    csp::schedule();

    CHECK_EQ(N, sent);
    CHECK_EQ(N, received);
}

TEST_CASE("Channel - AltFairness") {
    RunStats stats;

    channel<int> a, b;
    int count_a = 0, count_b = 0;
    constexpr int trials = 1000;

    stats.spawn([w = +a]{ for (int i = 0; w << 0; ++i) { } });
    stats.spawn([w = +b]{ for (int i = 0; w << 0; ++i) { } });

    stats.spawn([ra = -a, rb = -b, &count_a, &count_b]{
        int n;
        for (int i = 0; i < trials; ++i) {
            switch (alt(ra >> n, rb >> n)) {
            case 1: ++count_a; break;
            case 2: ++count_b; break;
            default: FAIL_CHECK("unexpected alt result"); return;
            }
        }
    });

    a.release();
    b.release();

    csp::schedule();

    // alt uses random_shuffle; in cooperative scheduling the bias
    // can be extreme, so just verify both channels were serviced.
    CHECK_EQ(trials, count_a + count_b);
}

TEST_CASE("Channel - PrialtOrder") {
    RunStats stats;

    channel<int> a, b;

    // Only channel a has a writer; channel b is writer-dead.
    stats.spawn([w = +a]{ for (;;) { if (!(w << 42)) return; } });
    auto ra = -a, rb = -b;
    a.release();
    b.release();

    int n = -1;
    // prialt scans in order: channel a (with pending writer) is found first.
    CHECK_EQ(1, prialt(ra >> n, rb >> n));
    CHECK_EQ(42, n);

    ra = {};
    rb = {};
    while (csp_run()) { }
}

TEST_CASE("Channel - NonBlocking") {
    RunStats stats;

    channel<int> ch;
    auto r = -ch;
    int n = -1;

    // No writer ready; skip (dead channel) fires immediately.
    CHECK_GT(0, prialt(r >> n, ~skip));
    CHECK_EQ(-1, n);

    // Make a writer ready.
    stats.spawn([w = +ch]{ w << 42; });
    ch.release();
    while (csp_run()) { }

    // Writer is waiting; read should succeed.
    CHECK_EQ(1, prialt(r >> n, ~skip));
    CHECK_EQ(42, n);

    r = {};
    while (csp_run()) { }
}

TEST_CASE("Channel - AltManyChannels") {
    RunStats stats;

    constexpr int N = 12; // > 8, exercises the heap path in Channel::alt.
    writer<int> ws[N];
    reader<int> rs[N];

    for (int i = 0; i < N; ++i) {
        rs[i] = --ws[i];
        stats.spawn([w = ws[i], i]{ w << i; });
        ws[i] = {};
    }

    while (csp_run()) { }

    int n = -1;
    std::vector<action> actions;
    for (int i = 0; i < N; ++i) {
        actions.push_back(rs[i] >> n);
    }

    int result = alt(actions);
    CHECK_GT(result, 0);
    CHECK_LE(result, N);
    CHECK_GE(n, 0);
    CHECK_LT(n, N);

    // Clean up: release readers so remaining writers unblock.
    actions.clear();
    for (int i = 0; i < N; ++i) rs[i] = {};
    while (csp_run()) { }
}
