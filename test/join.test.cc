#include "testutil.h"

#include <atomic>
#include <stdexcept>
#include <vector>

using namespace csp;

TEST_SUITE("join") {

TEST_CASE("single handle") {
    std::atomic<int> result{0};
    csp::run([&] {
        auto h = spawn([&] { result = 42; });
        join(h);
    });
    CHECK(result == 42);
}

TEST_CASE("vector of handles") {
    constexpr int N = 10;
    std::atomic<int> count{0};

    csp::run([&] {
        std::vector<reader<std::exception_ptr>> handles;
        for (int i = 0; i < N; i++) {
            handles.push_back(spawn([&] { count++; }));
        }
        join(handles);
    });
    CHECK(count == N);
}

TEST_CASE("variadic handles") {
    std::atomic<int> a{0}, b{0}, c{0};

    csp::run([&] {
        auto h1 = spawn([&] { a = 1; });
        auto h2 = spawn([&] { b = 2; });
        auto h3 = spawn([&] { c = 3; });
        join(std::move(h1), std::move(h2), std::move(h3));
    });
    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(c == 3);
}

TEST_CASE("vector - first exception rethrown after all complete") {
    constexpr int N = 5;
    std::atomic<int> finished{0};

    csp::run([&] {
        std::vector<reader<std::exception_ptr>> handles;
        for (int i = 0; i < N; i++) {
            handles.push_back(spawn([&, i] {
                if (i == 0) throw std::runtime_error("boom");
                finished++;
            }));
        }
        CHECK_THROWS_WITH_AS(join(handles), "boom", std::runtime_error);
    });
    CHECK(finished == N - 1);
}

TEST_CASE("variadic - first exception rethrown after all complete") {
    std::atomic<bool> second_done{false};

    csp::run([&] {
        auto h1 = spawn([&] { throw std::runtime_error("oops"); });
        auto h2 = spawn([&] { second_done = true; });
        CHECK_THROWS_WITH_AS(
            join(std::move(h1), std::move(h2)),
            "oops", std::runtime_error);
    });
    CHECK(second_done);
}

TEST_CASE("vector - multiple exceptions, first wins") {
    csp::run([&] {
        std::vector<reader<std::exception_ptr>> handles;
        handles.push_back(spawn([] { throw std::runtime_error("first"); }));
        handles.push_back(spawn([] { throw std::runtime_error("second"); }));
        CHECK_THROWS_WITH_AS(join(handles), "first", std::runtime_error);
    });
}

TEST_CASE("vector - no exceptions") {
    csp::run([&] {
        std::vector<reader<std::exception_ptr>> handles;
        for (int i = 0; i < 5; i++) {
            handles.push_back(spawn([] {}));
        }
        CHECK_NOTHROW(join(handles));
    });
}

TEST_CASE("vector - empty") {
    std::vector<reader<std::exception_ptr>> handles;
    CHECK_NOTHROW(join(handles));
}

}
