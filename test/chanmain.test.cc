#include "testutil.h"

#include <csp/buffer.h>

using namespace csp;

static Logger g_log("ChanMain.Test");

TEST_CASE("ChanMain - Write") {
    RunStats stats;

    chan<int> ch;
    int result = 0;

    stats.spawn([i = std::move(ch.r), &result]{
        i >> result;
    });

    auto o = std::move(ch.w);

    csp_run();
    o << 42;
    csp_run();

    CHECK_EQ(42, result);
}

TEST_CASE("ChanMain - Read") {
    RunStats stats;

    chan<int> ch;

    stats.spawn([o = std::move(ch.w)]{
        o << 42;
    });

    auto i = std::move(ch.r);

    // Give reader a chance to block on output.
    csp_run();

    int result = i.read();

    // Let reader exit.
    csp_run();

    CHECK_EQ(42, result);
}

auto worker = [](auto && o, auto && i) {
    return [o = std::move(o), i = std::move(i)]() mutable {
        for (int n = 1; n <= 5; ++n) {
            CHECK(bool(o << n));
        }

        o = {};

        int result = 0;
        int n;
        while (i >> n) {
            csp_run();
            result += n;
        };
        CHECK_EQ(15, result);
    };
};

// First confirm that it runs as a regular microthread.
TEST_CASE("ChanMain - WriteReadNormal") {
    RunStats stats;

    chan<int> a, b;

    stats.spawn(buffer(std::move(a.r), std::move(b.w), 5));
    stats.spawn(worker(std::move(a.w), std::move(b.r)));

    while (csp_run()) { }
}

// Now try from main.
TEST_CASE("ChanMain - WriteReadFromMain") {
    RunStats stats;

    chan<int> a, b;

    stats.spawn(buffer(std::move(a.r), std::move(b.w), 5));
    auto work = worker(std::move(a.w), std::move(b.r));

    csp_run();
    work();
}
