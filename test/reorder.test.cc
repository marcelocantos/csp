#include "testutil.h"

#include <utility>

using namespace csp;
using namespace csp::part;

TEST_SUITE("reorder") {

TEST_CASE("already-ordered") {
    RunStats stats;

    csp::run([&] {
        using P = std::pair<size_t, int>;
        chan<P> in;
        auto out = reorder<P>(std::function<size_t(P const&)>([](P const& p) { return p.first; }))
                       .spawn(std::move(in.r));

        spawn([w = std::move(in.w)] {
            for (size_t i = 0; i < 5; ++i) w << P{i, static_cast<int>(i * 10)};
        });

        std::vector<P> result;
        for (P v; out >> v;) result.push_back(std::move(v));
        REQUIRE(result.size() == 5);
        for (size_t i = 0; i < 5; ++i) {
            CHECK(result[i].first == i);
        }
    });
}

TEST_CASE("reversed-input") {
    RunStats stats;

    csp::run([&] {
        using P = std::pair<size_t, int>;
        chan<P> in;
        auto out = reorder<P>(std::function<size_t(P const&)>([](P const& p) { return p.first; }))
                       .spawn(std::move(in.r));

        spawn([w = std::move(in.w)] {
            for (int i = 4; i >= 0; --i) w << P{static_cast<size_t>(i), i * 10};
        });

        std::vector<P> result;
        for (P v; out >> v;) result.push_back(std::move(v));
        REQUIRE(result.size() == 5);
        for (size_t i = 0; i < 5; ++i) {
            CHECK(result[i].first == i);
        }
    });
}

TEST_CASE("interleaved-out-of-order") {
    RunStats stats;

    csp::run([&] {
        using P = std::pair<size_t, std::string>;
        chan<P> in;
        auto out = reorder<P>(std::function<size_t(P const&)>([](P const& p) { return p.first; }))
                       .spawn(std::move(in.r));

        spawn([w = std::move(in.w)] {
            w << P{0, "a"};
            w << P{2, "c"};
            w << P{1, "b"};
            w << P{4, "e"};
            w << P{3, "d"};
        });

        std::vector<P> result;
        for (P v; out >> v;) result.push_back(std::move(v));
        REQUIRE(result.size() == 5);
        CHECK(result[0].second == "a");
        CHECK(result[1].second == "b");
        CHECK(result[2].second == "c");
        CHECK(result[3].second == "d");
        CHECK(result[4].second == "e");
    });
}

TEST_CASE("empty-input") {
    RunStats stats;

    csp::run([&] {
        using P = std::pair<size_t, int>;
        auto out = reorder<P>(std::function<size_t(P const&)>([](P const& p) { return p.first; }))
                       .spawn(reader<P>::dead());
        P v;
        CHECK_FALSE(bool(out >> v));
    });
}

} // TEST_SUITE
