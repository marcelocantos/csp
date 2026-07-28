#include "testutil.h"

#include <string>
#include <vector>

using namespace csp;
using namespace csp::part;

TEST_CASE("ForeachEmit---running-sum-extracted-as-string") {
    RunStats stats;
    auto src = chan<int>();

    auto out = std::move(src.r)
        | foreach_emit<int, int, std::string>(
            0,
            [](int acc, int n) { return acc + n; },
            [](int acc) { return std::to_string(acc); });

    stats.spawn([w = std::move(src.w)]{ w << 1; w << 2; w << 3; });

    std::vector<std::string> got;
    stats.spawn([r = std::move(out), &got] {
        for (std::string s; r >> s;) got.push_back(std::move(s));
    });
    csp::await_completion();

    CHECK(got == std::vector<std::string>{"1", "3", "6"});
}

TEST_CASE("ForeachEmit---extract-subset-of-state") {
    RunStats stats;
    auto src = chan<int>();

    // State is (count, sum), extract is the running average.
    using State = std::pair<int, int>;
    auto out = std::move(src.r)
        | foreach_emit<int, State, double>(
            State{0, 0},
            [](State s, int n) { return State{s.first + 1, s.second + n}; },
            [](State const& s) { return static_cast<double>(s.second) / s.first; });

    stats.spawn([w = std::move(src.w)]{ w << 10; w << 20; w << 30; });

    std::vector<double> got;
    stats.spawn([r = std::move(out), &got] {
        for (double d; r >> d;) got.push_back(d);
    });
    csp::await_completion();

    REQUIRE(got.size() == 3);
    CHECK(got[0] == doctest::Approx(10.0));
    CHECK(got[1] == doctest::Approx(15.0));
    CHECK(got[2] == doctest::Approx(20.0));
}

TEST_CASE("ForeachEmit---empty-input") {
    RunStats stats;
    auto src = chan<int>();

    auto out = std::move(src.r)
        | foreach_emit<int, int, int>(
            0,
            [](int acc, int n) { return acc + n; },
            [](int acc) { return acc; });

    stats.spawn([w = std::move(src.w)]{ });

    std::vector<int> got;
    stats.spawn([r = std::move(out), &got] {
        for (int v; r >> v;) got.push_back(v);
    });
    csp::await_completion();

    CHECK(got.empty());
}
