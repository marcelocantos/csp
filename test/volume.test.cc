#include "testutil.h"
#include "testscale.h"

#include <atomic>

using namespace csp;

static Logger g_log("Channel.Test");

TEST_CASE("Volume---Megaloop") {
    constexpr int n_loops = 1000000 / SCALE_HEAVY;
    int total = 0;

    csp::run([&]{
        auto ch = spawn_filter<int>([](auto && r, auto && w) {
            for (int n; r >> n && w << (n + 1);) { }
        });

        for (int i = 0; i < n_loops; ++i) {
            ch.w.copy() << 0;
            total += ch.r.copy().read();
        }
        ch.release();
    });
    CHECK(n_loops == total);
}

TEST_CASE("Volume---DaisyChain") {
    constexpr int n_threads = 100 / SCALE_LIGHT;
    constexpr int n_loops = 10000 / SCALE_MEDIUM;
    int total = 0;

    csp::run([&]{
        chan<int> ch;
        auto tail = std::move(ch.r);
        for (int i = 0; i < n_threads; ++i) {
            tail = spawn_producer<int>([r = std::move(tail)](auto && w) {
                for (int n; r >> n && w << (n + 1);) { }
            });
        }

        for (int i = 0; i < n_loops; ++i) {
            ch.w.copy() << 0;
            total += tail.read();
        }
        ch = {};
    });
    CHECK(n_threads * n_loops == total);
}

TEST_CASE("Volume---RapidChannelLifecycle") {
    constexpr int N = 10000 / SCALE_MEDIUM;

    int before_w = csp::internal::channel_count(0);
    int before_r = csp::internal::channel_count(1);

    for (int i = 0; i < N; ++i) {
        chan<int> ch;
    }

    CHECK(before_w == csp::internal::channel_count(0));
    CHECK(before_r == csp::internal::channel_count(1));
}

TEST_CASE("Volume---ManyImps") {
    constexpr int N = 2000 / SCALE_LIGHT;
    std::atomic<int> completed{0};

    csp::run([&]{
        for (int i = 0; i < N; ++i) {
            csp::spawn([&]{ ++completed; });
        }
    });

    CHECK(N == completed.load());
    CHECK(0 == csp::internal::channel_count(0));
    CHECK(0 == csp::internal::channel_count(1));
}

TEST_CASE("Volume---ManyChannelPairs") {
    constexpr int N = 500 / SCALE_LIGHT;
    std::atomic<int> total{0};

    csp::run([&]{
        for (int i = 0; i < N; ++i) {
            chan<int> ch;
            csp::spawn([w = ch.w.copy(), i]{ w << i; });
            csp::spawn([r = ch.r.copy(), &total]{ int n; if (r >> n) total += n; });
            ch.release();
        }
    });

    CHECK(N * (N - 1) / 2 == total.load());
    CHECK(0 == csp::internal::channel_count(0));
    CHECK(0 == csp::internal::channel_count(1));
}
