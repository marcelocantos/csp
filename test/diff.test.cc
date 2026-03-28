#include "testutil.h"

using namespace csp;
using namespace csp::part;

TEST_SUITE("diff") {

TEST_CASE("basic differences") {
    RunStats stats;

    chan<int> in;
    auto out = diff<int>.spawn(std::move(in.r));

    spawn([w = std::move(in.w)] {
        for (int v : {1, 4, 6, 4, 1}) w << v;
    });

    std::vector<int> result;
    for (int v; out >> v;) result.push_back(v);
    CHECK(result == std::vector<int>{3, 2, -2, -3});
}

TEST_CASE("single element - no output") {
    RunStats stats;

    chan<int> in;
    auto out = diff<int>.spawn(std::move(in.r));

    spawn([w = std::move(in.w)] { w << 42; });

    std::vector<int> result;
    for (int v; out >> v;) result.push_back(v);
    CHECK(result.empty());
}

TEST_CASE("empty input") {
    RunStats stats;

    auto out = diff<int>.spawn(reader<int>::dead());
    int v;
    CHECK_FALSE(bool(out >> v));
}

TEST_CASE("constant input - all zeros") {
    RunStats stats;

    chan<int> in;
    auto out = diff<int>.spawn(std::move(in.r));

    spawn([w = std::move(in.w)] {
        for (int v : {5, 5, 5, 5}) w << v;
    });

    std::vector<int> result;
    for (int v; out >> v;) result.push_back(v);
    CHECK(result == std::vector<int>{0, 0, 0});
}

TEST_CASE("difference triangle") {
    RunStats stats;

    // Pascal's triangle row: 1 4 6 4 1
    // Repeated diff peels off layers.
    std::vector<int> row = {1, 4, 6, 4, 1};
    std::vector<std::vector<int>> triangle;

    while (row.size() > 1) {
        chan<int> in;
        auto out = diff<int>.spawn(std::move(in.r));
        spawn([&row, w = std::move(in.w)] {
            for (int v : row) w << v;
        });
        row.clear();
        for (int v; out >> v;) row.push_back(v);
        triangle.push_back(row);
    }

    CHECK(triangle[0] == std::vector<int>{3, 2, -2, -3});
    CHECK(triangle[1] == std::vector<int>{-1, -4, -1});
    CHECK(triangle[2] == std::vector<int>{-3, 3});
    CHECK(triangle[3] == std::vector<int>{6});
}

} // TEST_SUITE
