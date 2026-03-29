#include "testutil.h"

using namespace csp;
using namespace csp::part;
using namespace std::chrono_literals;

TEST_SUITE("frame") {

TEST_CASE("batch fills before timeout") {
    RunStats stats;

    csp::run([&] {
        chan<int> in;
        auto out = frame<int>(3, 1s).spawn(std::move(in.r));

        spawn([w = std::move(in.w)] {
            for (int i = 0; i < 6; ++i) w << i;
        });

        std::vector<std::vector<int>> frames;
        std::vector<int> f;
        while (out >> f) frames.push_back(std::move(f));

        REQUIRE(frames.size() == 2);
        CHECK(frames[0] == std::vector<int>{0, 1, 2});
        CHECK(frames[1] == std::vector<int>{3, 4, 5});
    });
}

TEST_CASE("partial frame flushed on input close") {
    RunStats stats;

    csp::run([&] {
        chan<int> in;
        auto out = frame<int>(5, 1s).spawn(std::move(in.r));

        spawn([w = std::move(in.w)] {
            for (int i = 0; i < 3; ++i) w << i;
        });

        std::vector<std::vector<int>> frames;
        std::vector<int> f;
        while (out >> f) frames.push_back(std::move(f));

        REQUIRE(frames.size() == 1);
        CHECK(frames[0] == std::vector<int>{0, 1, 2});
    });
}

TEST_CASE("timeout flushes partial frame") {
    RunStats stats;

    chan<int> in;
    auto out = frame<int>(100, 50ms).spawn(std::move(in.r));

    spawn([w = std::move(in.w)] {
        w << 1;
        w << 2;
        csp::sleep(200ms);
        w << 3;
        w << 4;
        csp::sleep(200ms);
    });

    // Read from a spawned imp to avoid main-thread channel operations
    // (the frame's timeout creates timer channels that require scheduling).
    std::vector<std::vector<int>> frames;
    spawn([&frames, out = std::move(out)]() mutable {
        std::vector<int> f;
        while (out >> f) frames.push_back(std::move(f));
    });

    schedule();

    CHECK(frames.size() >= 2);
    CHECK(frames[0] == std::vector<int>{1, 2});
}

TEST_CASE("empty input") {
    RunStats stats;

    csp::run([&] {
        auto out = frame<int>(3, 1s).spawn(reader<int>::dead());

        std::vector<int> f;
        CHECK_FALSE(bool(out >> f));
    });
}

} // TEST_SUITE
