#include "testutil.h"

#include <csp/buffer.h>

using namespace csp;

// TODO: test more buffer edge-cases.

TEST_CASE("ChanUtil - BufferBounded") {
    RunStats stats;

    auto ch = spawn_buffer<int>(5);

    int sent = 0;

    stats.spawn([out = std::move(ch.w), &sent]{
        for (int i = 1; i <= 10; ++i) {
            REQUIRE(bool(out << i));
            sent += i;
        }
    });

    while (csp::internal::run()) { }
    CHECK_EQ(0UL, stats.pending());

    REQUIRE_EQ(15, sent);

    int received = 0;

    stats.spawn([in = std::move(ch.r), &received]{
        int n;
        while (in >> n) {
            received += n;
        }
    });

    while (csp::internal::run()) { }
    CHECK_EQ(55, sent);
    CHECK_EQ(55, received);
}

TEST_CASE("ChanUtil - BufferUnbounded") {
    RunStats stats;

    int sent = 0;
    int received = 0;

    auto [send_w, send_r] = chan<>{};
    auto [recv_w, recv_r] = chan<>{};

    auto buf = spawn_buffer<int>();

    stats.spawn([trigger = std::move(send_r), out = std::move(buf.w), &sent]{
        for (int i = 0; trigger >> poke; ++i) {
            out << i;
            sent += 1;
        }
    });

    stats.spawn([trigger = std::move(recv_r), in = std::move(buf.r), &received]{
        for (int i = 0; trigger >> poke; ++i) {
            CHECK_EQ(i, in.read());
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

    CHECK_EQ(55, sent);
    CHECK_EQ(55, received);
}

TEST_CASE("ChanUtil - BufferEmpty") {
    RunStats stats;

    auto ch = spawn_buffer<int>(5);

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
    CHECK_EQ(0, received);
}

TEST_CASE("ChanUtil - BufferSingle") {
    RunStats stats;

    auto ch = spawn_buffer<int>(1);

    stats.spawn([out = std::move(ch.w)]{
        for (int i = 1; i <= 5; ++i) {
            REQUIRE(bool(out << i));
        }
    });

    stats.spawn([in = std::move(ch.r)]{
        for (int i = 1; i <= 5; ++i) {
            CHECK_EQ(i, in.read());
        }
    });

    ch.release();
    csp::schedule();
}
