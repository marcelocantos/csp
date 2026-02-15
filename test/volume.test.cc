#include "testutil.h"
#include "testscale.h"

using namespace csp;

static Logger g_log("Channel.Test");

TEST_CASE("Volume - Megaloop") {
    constexpr int n_loops = 1000000 / SCALE_HEAVY;

    channel<int> in, out;
    auto ch = spawn_filter<int>([](auto && r, auto && w) {
        for (int n; r >> n && w << (n + 1);) { }
    });

    int total = 0;
    for (int i = 0; i < n_loops; ++i) {
        +ch << 0;
        total += (-ch).read();
    }
    ch.release();
    CHECK_EQ(n_loops, total);
}

TEST_CASE("Volume - DaisyChain") {
    constexpr int n_threads = 100 / SCALE_LIGHT;
    constexpr int n_loops = 10000 / SCALE_MEDIUM;

    channel<int> ch;
    auto tail = --ch;
    for (int i = 0; i < n_threads; ++i) {
        tail = spawn_producer<int>([r = std::move(tail)](auto && w) {
            for (int n; r >> n && w << (n + 1);) { }
        });
    }

    int total = 0;
    for (int i = 0; i < n_loops; ++i) {
        +ch << 0;
        total += tail.read();
    }
    ch = {};
    CHECK_EQ(n_threads * n_loops, total);
}

TEST_CASE("Volume - RapidChannelLifecycle") {
    constexpr int N = 10000 / SCALE_MEDIUM;

    int before_w = csp__internal__channel_count(0);
    int before_r = csp__internal__channel_count(1);

    for (int i = 0; i < N; ++i) {
        channel<int> ch;
    }

    CHECK_EQ(before_w, csp__internal__channel_count(0));
    CHECK_EQ(before_r, csp__internal__channel_count(1));
}

TEST_CASE("Volume - ManyMicrothreads") {
    constexpr int N = 2000 / SCALE_LIGHT;
    int completed = 0;

    for (int i = 0; i < N; ++i) {
        csp::spawn([&]{ ++completed; });
    }

    while (csp_run()) { }

    CHECK_EQ(N, completed);
    CHECK_EQ(0, csp__internal__channel_count(0));
    CHECK_EQ(0, csp__internal__channel_count(1));
}

TEST_CASE("Volume - ManyChannelPairs") {
    constexpr int N = 500 / SCALE_LIGHT;
    int total = 0;

    for (int i = 0; i < N; ++i) {
        channel<int> ch;
        csp::spawn([w = +ch, i]{ w << i; });
        csp::spawn([r = -ch, &total]{ int n; if (r >> n) total += n; });
        ch.release();
    }

    while (csp_run()) { }

    CHECK_EQ(N * (N - 1) / 2, total);
    CHECK_EQ(0, csp__internal__channel_count(0));
    CHECK_EQ(0, csp__internal__channel_count(1));
}
