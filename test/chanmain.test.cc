#include "testutil.h"

#include <csp/part/buffer.h>

using namespace csp;
using namespace csp::part;

static Logger g_log("ChanMain.Test");

TEST_CASE("ChanMain - Write") {
    RunStats stats;

    auto [w, r] = chan<int>{};
    int result = 0;

    stats.spawn([i = std::move(r), &result]{
        i >> result;
    });

    auto o = std::move(w);

    csp::internal::run();
    o << 42;
    csp::internal::run();

    CHECK_EQ(42, result);
}

TEST_CASE("ChanMain - Read") {
    RunStats stats;

    auto [w, r] = chan<int>{};

    stats.spawn([o = std::move(w)]{
        o << 42;
    });

    auto i = std::move(r);

    // Give reader a chance to block on output.
    csp::internal::run();

    int result = i.read();

    // Let reader exit.
    csp::internal::run();

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
            csp::internal::run();
            result += n;
        };
        CHECK_EQ(15, result);
    };
};

// First confirm that it runs as a regular microthread.
TEST_CASE("ChanMain - WriteReadNormal") {
    RunStats stats;

    auto [a_w, a_r] = chan<int>{};
    auto [b_w, b_r] = chan<int>{};

    stats.spawn(buffer<int>(5).bind(std::move(a_r), std::move(b_w)));
    stats.spawn(worker(std::move(a_w), std::move(b_r)));

    while (csp::internal::run()) { }
}

// Now try from main.
TEST_CASE("ChanMain - WriteReadFromMain") {
    RunStats stats;

    auto [a_w, a_r] = chan<int>{};
    auto [b_w, b_r] = chan<int>{};

    stats.spawn(buffer<int>(5).bind(std::move(a_r), std::move(b_w)));
    auto work = worker(std::move(a_w), std::move(b_r));

    csp::internal::run();
    work();
}
