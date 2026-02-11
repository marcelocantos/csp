#include "testutil.h"

#include <csp/buffer.h>

using namespace csp;

// TODO: test more buffer edge-cases.

TEST_CASE("ChanUtil - BufferBounded") {
    RunStats stats;

    auto ch = chan::spawn_buffer<int>(5);

    int sent = 0;

    stats.spawn([out = ++ch, &sent]{
        for (int i = 1; i <= 10; ++i) {
            REQUIRE(bool(out << i));
            sent += i;
        }
    });

    while (csp_run()) { }
    CHECK_EQ(0UL, stats.pending());

    REQUIRE_EQ(15, sent);

    int received = 0;

    stats.spawn([in = --ch, &received]{
        int n;
        while (in >> n) {
            received += n;
        }
    });

    while (csp_run()) { }
    CHECK_EQ(55, sent);
    CHECK_EQ(55, received);
}

TEST_CASE("ChanUtil - BufferUnbounded") {
    RunStats stats;

    int sent = 0;
    int received = 0;

    channel<> send, recv;

    auto buf = chan::spawn_buffer<int>();

    stats.spawn([trigger = --send, out = ++buf, &sent]{
        for (int i = 0; trigger >> poke; ++i) {
            out << i;
            sent += 1;
        }
    });

    stats.spawn([trigger = --recv, in = --buf, &received]{
        for (int i = 0; trigger >> poke; ++i) {
            CHECK_EQ(i, in.read());
            received += 1;
        }
    });

    stats.spawn([send = ++send, recv = ++recv, &sent, &received] {
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

    while (csp_run()) { }

    CHECK_EQ(55, sent);
    CHECK_EQ(55, received);
}

TEST_CASE("ChanUtil - BufferEmpty") {
    RunStats stats;

    auto ch = chan::spawn_buffer<int>(5);

    // Writer dies immediately without sending anything.
    stats.spawn([out = ++ch]{ });

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

TEST_CASE("ChanUtil - BufferSingle") {
    RunStats stats;

    auto ch = chan::spawn_buffer<int>(1);

    stats.spawn([out = ++ch]{
        for (int i = 1; i <= 5; ++i) {
            REQUIRE(bool(out << i));
        }
    });

    stats.spawn([in = --ch]{
        for (int i = 1; i <= 5; ++i) {
            CHECK_EQ(i, in.read());
        }
    });

    ch.release();
    csp::schedule();
}
