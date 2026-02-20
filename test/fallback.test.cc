#include "testutil.h"

#include <vector>

using namespace csp;
using namespace csp::part;

TEST_CASE("Fallback - first reader produces") {
    RunStats stats;
    auto a = chan<int>();
    auto b = chan<int>();

    std::vector<reader<int>> inputs;
    inputs.push_back(std::move(a.r));
    inputs.push_back(std::move(b.r));
    auto out = fallback(std::move(inputs)).spawn();

    stats.spawn([w = std::move(a.w)]{ w << 1; w << 2; });
    stats.spawn([w = std::move(b.w)]{ w << 10; w << 20; });

    std::vector<int> got;
    stats.spawn([r = std::move(out), &got] {
        for (int v; r >> v;) got.push_back(v);
    });
    csp::schedule();

    CHECK_EQ(got, std::vector<int>{1, 2});
}

TEST_CASE("Fallback - first empty, second produces") {
    RunStats stats;
    auto a = chan<int>();
    auto b = chan<int>();

    std::vector<reader<int>> inputs;
    inputs.push_back(std::move(a.r));
    inputs.push_back(std::move(b.r));
    auto out = fallback(std::move(inputs)).spawn();

    stats.spawn([w = std::move(a.w)]{ });  // closes immediately
    stats.spawn([w = std::move(b.w)]{ w << 10; w << 20; });

    std::vector<int> got;
    stats.spawn([r = std::move(out), &got] {
        for (int v; r >> v;) got.push_back(v);
    });
    csp::schedule();

    CHECK_EQ(got, std::vector<int>{10, 20});
}

TEST_CASE("Fallback - all empty") {
    RunStats stats;
    auto a = chan<int>();
    auto b = chan<int>();

    std::vector<reader<int>> inputs;
    inputs.push_back(std::move(a.r));
    inputs.push_back(std::move(b.r));
    auto out = fallback(std::move(inputs)).spawn();

    stats.spawn([w = std::move(a.w)]{ });
    stats.spawn([w = std::move(b.w)]{ });

    std::vector<int> got;
    stats.spawn([r = std::move(out), &got] {
        for (int v; r >> v;) got.push_back(v);
    });
    csp::schedule();

    CHECK(got.empty());
}

TEST_CASE("Fallback - single reader") {
    RunStats stats;
    auto a = chan<int>();

    std::vector<reader<int>> inputs;
    inputs.push_back(std::move(a.r));
    auto out = fallback(std::move(inputs)).spawn();

    stats.spawn([w = std::move(a.w)]{ w << 42; });

    std::vector<int> got;
    stats.spawn([r = std::move(out), &got] {
        for (int v; r >> v;) got.push_back(v);
    });
    csp::schedule();

    CHECK_EQ(got, std::vector<int>{42});
}
