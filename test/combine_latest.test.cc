#include "testutil.h"

#include <string>
#include <tuple>
#include <vector>

using namespace csp;
using namespace csp::part;

TEST_SUITE("CombineLatest") {

TEST_CASE("Two-inputs---basic") {
    RunStats stats;

    auto [aw, ar] = chan<int>{};
    auto [bw, br] = chan<std::string>{};
    auto r = combine_latest(std::move(ar), std::move(br)).spawn();

    using T = std::tuple<int, std::string>;
    std::vector<T> got;

    stats.spawn([aw = std::move(aw), bw = std::move(bw)]{
        aw << 1;     // No emission (b not yet initialised).
        bw << "x";   // Emits (1, "x").
        aw << 2;     // Emits (2, "x").
        bw << "y";   // Emits (2, "y").
    });

    stats.spawn([r = std::move(r), &got]{
        for (T t; r >> t;) got.push_back(std::move(t));
    });

    csp::await_completion();
    REQUIRE(3u == got.size());
    CHECK(T(1, "x") == got[0]);
    CHECK(T(2, "x") == got[1]);
    CHECK(T(2, "y") == got[2]);
}

TEST_CASE("No-emission-until-all-inputs-produce") {
    RunStats stats;

    auto [aw, ar] = chan<int>{};
    auto [bw, br] = chan<int>{};
    auto r = combine_latest(std::move(ar), std::move(br)).spawn();

    std::vector<std::tuple<int, int>> got;

    stats.spawn([aw = std::move(aw), bw = std::move(bw)]{
        aw << 1;
        aw << 2;
        aw << 3;
        bw << 10;  // First emission: (3, 10).
    });

    stats.spawn([r = std::move(r), &got]{
        using T = std::tuple<int, int>;
        for (T t; r >> t;) got.push_back(t);
    });

    csp::await_completion();
    REQUIRE(1u == got.size());
    CHECK(std::make_tuple(3, 10) == got[0]);
}

TEST_CASE("Input-dies-after-producing---retains-last-value") {
    RunStats stats;

    auto [aw, ar] = chan<int>{};
    auto [bw, br] = chan<int>{};
    auto r = combine_latest(std::move(ar), std::move(br)).spawn();

    std::vector<std::tuple<int, int>> got;

    stats.spawn([aw = std::move(aw), bw = std::move(bw)]{
        aw << 1;
        bw << 10;   // Emits (1, 10).
        // aw closes here.
        // bw sends more.
        writer<int> aw2; // null — force aw to close.
        bw << 20;   // Emits (1, 20).
        bw << 30;   // Emits (1, 30).
    });

    stats.spawn([r = std::move(r), &got]{
        using T = std::tuple<int, int>;
        for (T t; r >> t;) got.push_back(t);
    });

    csp::await_completion();
    REQUIRE(3u == got.size());
    CHECK(std::make_tuple(1, 10) == got[0]);
    CHECK(std::make_tuple(1, 20) == got[1]);
    CHECK(std::make_tuple(1, 30) == got[2]);
}

TEST_CASE("Input-dies-before-producing---output-closes") {
    RunStats stats;

    auto [aw, ar] = chan<int>{};
    auto [bw, br] = chan<int>{};
    auto r = combine_latest(std::move(ar), std::move(br)).spawn();

    stats.spawn([bw = std::move(bw)]{
        bw << 10;
        bw << 20;
        // aw never gets a writer — already dead (aw dropped outside spawn).
    });

    // Drop aw immediately so input 0 is dead before producing.
    aw = {};

    stats.spawn([r = std::move(r)]{
        std::tuple<int, int> t;
        CHECK_FALSE(bool(r >> t));
    });

    csp::await_completion();
}

TEST_CASE("Output-death-stops") {
    RunStats stats;

    auto [aw, ar] = chan<int>{};
    auto [bw, br] = chan<int>{};
    auto r = combine_latest(std::move(ar), std::move(br)).spawn();

    stats.spawn([aw = std::move(aw), bw = std::move(bw)]{
        aw << 1;
        bw << 2;
        // Output reader will be dropped after first read.
        // Subsequent writes may fail.
        aw << 3;
    });

    stats.spawn([r = std::move(r)]{
        using T = std::tuple<int, int>;
        T t;
        CHECK(bool(r >> t));
        CHECK(std::make_tuple(1, 2) == t);
        // Drop reader — output dies.
    });

    csp::await_completion();
}

TEST_CASE("Three-inputs") {
    RunStats stats;

    auto [aw, ar] = chan<int>{};
    auto [bw, br] = chan<double>{};
    auto [cw, cr] = chan<std::string>{};
    auto r = combine_latest(std::move(ar), std::move(br), std::move(cr)).spawn();

    using T = std::tuple<int, double, std::string>;
    std::vector<T> got;

    stats.spawn([aw = std::move(aw), bw = std::move(bw), cw = std::move(cw)]{
        aw << 1;
        bw << 2.5;
        cw << "hi";  // First emission: (1, 2.5, "hi").
        aw << 3;     // Emits (3, 2.5, "hi").
    });

    stats.spawn([r = std::move(r), &got]{
        for (T t; r >> t;) got.push_back(std::move(t));
    });

    csp::await_completion();
    REQUIRE(2u == got.size());
    CHECK(T(1, 2.5, "hi") == got[0]);
    CHECK(T(3, 2.5, "hi") == got[1]);
}

TEST_CASE("Combining-function") {
    RunStats stats;

    auto [aw, ar] = chan<int>{};
    auto [bw, br] = chan<int>{};
    auto r = combine_latest<int, int>(
        std::move(ar), std::move(br),
        [](int a, int b) { return a + b; }).spawn();

    std::vector<int> got;

    stats.spawn([aw = std::move(aw), bw = std::move(bw)]{
        aw << 10;
        bw << 1;   // Emits 11.
        aw << 20;  // Emits 21.
        bw << 2;   // Emits 22.
    });

    stats.spawn([r = std::move(r), &got]{
        for (int v; r >> v;) got.push_back(v);
    });

    csp::await_completion();
    REQUIRE(3u == got.size());
    CHECK(11 == got[0]);
    CHECK(21 == got[1]);
    CHECK(22 == got[2]);
}

}
