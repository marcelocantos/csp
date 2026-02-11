#include "testutil.h"

#include <csp/buffer.h>

using namespace csp;

static Logger g_log("ChanMain.Test");

TEST_CASE("ChanMain - Write") {
    RunStats stats;

    channel<int> ch;
    int result = 0;

    stats.spawn([i = --ch, &result]{
        i >> result;
    });

    auto o = ++ch;

    csp_run();
    o << 42;
    csp_run();

    CHECK_EQ(42, result);
}

TEST_CASE("ChanMain - Read") {
    RunStats stats;

    channel<int> ch;

    stats.spawn([o = ++ch]{
        o << 42;
    });

    auto i = --ch;

    // Give reader a chance to block on output.
    csp_run();

    int result = i.read();

    // Let reader exit.
    csp_run();

    CHECK_EQ(42, result);
}

auto worker = [](auto && o, auto && i) {
    return [o = std::make_shared<writer<int>>(std::move(o)), i = std::move(i)]{
        tracer<int>::set(4);
        for (int n = 1; n <= 5; ++n) {
            CHECK(bool(*o << n));
        }

        *o = {};

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

    channel<int> a, b;

    stats.spawn(chan::buffer(--a, ++b, 5));
    stats.spawn(worker(++a, --b));

    while (csp_run()) { }
}

// Now try from main.
TEST_CASE("ChanMain - WriteReadFromMain") {
    RunStats stats;

    channel<int> a, b;

    stats.spawn(chan::buffer(--a, ++b, 5));
    auto work = worker(++a, --b);

    csp_run();
    work();
}
