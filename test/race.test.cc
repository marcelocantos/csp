#include "testutil.h"

using namespace csp;
using namespace csp::part;

TEST_SUITE("race") {

TEST_CASE("single-source") {
    RunStats stats;

    csp::run([&] {
        chan<int> a;
        spawn([w = std::move(a.w)] {
            for (int i = 0; i < 3; ++i) w << i;
        });

        std::vector<reader<int>> sources;
        sources.push_back(std::move(a.r));
        auto out = race(std::move(sources));

        std::vector<int> result;
        for (int v; out >> v;) result.push_back(v);
        CHECK(result == std::vector<int>{0, 1, 2});
    });
}

TEST_CASE("multiple-sources---all-values-received") {
    RunStats stats;

    csp::run([&] {
        chan<int> a, b;
        spawn([w = std::move(a.w)] {
            for (int i = 0; i < 3; ++i) w << i;
        });
        spawn([w = std::move(b.w)] {
            for (int i = 10; i < 13; ++i) w << i;
        });

        std::vector<reader<int>> sources;
        sources.push_back(std::move(a.r));
        sources.push_back(std::move(b.r));
        auto out = race(std::move(sources));

        std::vector<int> result;
        for (int v; out >> v;) result.push_back(v);
        CHECK(result.size() == 6);
    });
}

TEST_CASE("empty-sources") {
    RunStats stats;

    csp::run([&] {
        auto out = race(std::vector<reader<int>>{});

        int v;
        CHECK_FALSE(bool(out >> v));
    });
}

TEST_CASE("one-source-dies-early") {
    RunStats stats;

    csp::run([&] {
        chan<int> a, b;
        spawn([w = std::move(a.w)] { w << 1; });
        spawn([w = std::move(b.w)] {
            for (int i = 10; i < 15; ++i) w << i;
        });

        std::vector<reader<int>> sources;
        sources.push_back(std::move(a.r));
        sources.push_back(std::move(b.r));
        auto out = race(std::move(sources));

        std::vector<int> result;
        for (int v; out >> v;) result.push_back(v);
        CHECK(result.size() == 6);
    });
}

} // TEST_SUITE
