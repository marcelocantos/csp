#include "testutil.h"

#include <vector>

using namespace csp;
using namespace csp::part;

TEST_CASE("Transpose - 3x3 matrix") {
    RunStats stats;
    auto a = chan<int>();
    auto b = chan<int>();
    auto c = chan<int>();

    std::vector<reader<int>> inputs;
    inputs.push_back(std::move(a.r));
    inputs.push_back(std::move(b.r));
    inputs.push_back(std::move(c.r));
    auto out = transpose(std::move(inputs)).spawn();

    stats.spawn([w = std::move(a.w)]{ w << 1; w << 4; w << 7; });
    stats.spawn([w = std::move(b.w)]{ w << 2; w << 5; w << 8; });
    stats.spawn([w = std::move(c.w)]{ w << 3; w << 6; w << 9; });

    std::vector<std::vector<int>> got;
    stats.spawn([r = std::move(out), &got] {
        for (std::vector<int> v; r >> v;) got.push_back(std::move(v));
    });
    csp::schedule();

    CHECK(got == std::vector<std::vector<int>>{{1, 2, 3}, {4, 5, 6}, {7, 8, 9}});
}

TEST_CASE("Transpose - unequal lengths stops at shortest") {
    RunStats stats;
    auto a = chan<int>();
    auto b = chan<int>();

    std::vector<reader<int>> inputs;
    inputs.push_back(std::move(a.r));
    inputs.push_back(std::move(b.r));
    auto out = transpose(std::move(inputs)).spawn();

    stats.spawn([w = std::move(a.w)]{ w << 1; w << 3; w << 5; });
    stats.spawn([w = std::move(b.w)]{ w << 2; w << 4; });  // shorter

    std::vector<std::vector<int>> got;
    stats.spawn([r = std::move(out), &got] {
        for (std::vector<int> v; r >> v;) got.push_back(std::move(v));
    });
    csp::schedule();

    CHECK(got == std::vector<std::vector<int>>{{1, 2}, {3, 4}});
}

TEST_CASE("Transpose - single input") {
    RunStats stats;
    auto a = chan<int>();

    std::vector<reader<int>> inputs;
    inputs.push_back(std::move(a.r));
    auto out = transpose(std::move(inputs)).spawn();

    stats.spawn([w = std::move(a.w)]{ w << 10; w << 20; });

    std::vector<std::vector<int>> got;
    stats.spawn([r = std::move(out), &got] {
        for (std::vector<int> v; r >> v;) got.push_back(std::move(v));
    });
    csp::schedule();

    CHECK(got == std::vector<std::vector<int>>{{10}, {20}});
}

TEST_CASE("Transpose - empty inputs") {
    RunStats stats;
    auto a = chan<int>();
    auto b = chan<int>();

    std::vector<reader<int>> inputs;
    inputs.push_back(std::move(a.r));
    inputs.push_back(std::move(b.r));
    auto out = transpose(std::move(inputs)).spawn();

    stats.spawn([w = std::move(a.w)]{ });
    stats.spawn([w = std::move(b.w)]{ });

    std::vector<std::vector<int>> got;
    stats.spawn([r = std::move(out), &got] {
        for (std::vector<int> v; r >> v;) got.push_back(std::move(v));
    });
    csp::schedule();

    CHECK(got.empty());
}
