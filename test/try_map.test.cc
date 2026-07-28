#include "testutil.h"

#include <stdexcept>
#include <string>
#include <vector>

using namespace csp;
using namespace csp::part;

TEST_CASE("TryMap---all-succeed") {
    RunStats stats;
    auto src = chan<int>();
    auto err = chan<std::exception_ptr>();

    auto out = std::move(src.r) | try_map<int>([](int n) { return n * 2; }, std::move(err.w));

    stats.spawn([w = std::move(src.w)]{ w << 1; w << 2; w << 3; });
    stats.spawn([r = std::move(out)] {
        CHECK(r.read() == 2);
        CHECK(r.read() == 4);
        CHECK(r.read() == 6);
    });
    stats.spawn([r = std::move(err.r)] {
        std::exception_ptr ep;
        CHECK(!(r >> ep));  // no errors, channel closes
    });
    csp::await_completion();
}

TEST_CASE("TryMap---exceptions-to-error-channel") {
    RunStats stats;
    auto src = chan<int>();
    auto err = chan<std::exception_ptr>();

    auto out = std::move(src.r) | try_map<int>([](int n) -> int {
        if (n < 0) throw std::runtime_error("negative");
        return n * 2;
    }, std::move(err.w));

    stats.spawn([w = std::move(src.w)]{ w << 1; w << -1; w << 2; w << -2; });

    std::vector<int> results;
    std::vector<std::string> errors;

    stats.spawn([r = std::move(out), &results] {
        for (int v; r >> v;) results.push_back(v);
    });
    stats.spawn([r = std::move(err.r), &errors] {
        for (std::exception_ptr ep; r >> ep;) {
            try { std::rethrow_exception(ep); }
            catch (std::exception const& e) { errors.push_back(e.what()); }
        }
    });
    csp::await_completion();

    CHECK(results == std::vector<int>{2, 4});
    CHECK(errors == std::vector<std::string>{"negative", "negative"});
}

TEST_CASE("TryMap---no-error-channel-(exceptions-propagate)") {
    RunStats stats;
    auto src = chan<int>();
    auto out = std::move(src.r) | try_map<int>([](int n) { return n + 1; });

    stats.spawn([w = std::move(src.w)]{ w << 10; });
    stats.spawn([r = std::move(out)] {
        CHECK(r.read() == 11);
    });
    csp::await_completion();
}

TEST_CASE("TryMap---type-changing-transform") {
    RunStats stats;
    auto src = chan<int>();
    auto err = chan<std::exception_ptr>();

    auto out = std::move(src.r) | try_map<int, std::string>([](int n) -> std::string {
        if (n == 0) throw std::runtime_error("zero");
        return std::to_string(n);
    }, std::move(err.w));

    stats.spawn([w = std::move(src.w)]{ w << 42; w << 0; w << 7; });

    std::vector<std::string> results;
    int error_count = 0;

    stats.spawn([r = std::move(out), &results] {
        for (std::string s; r >> s;) results.push_back(std::move(s));
    });
    stats.spawn([r = std::move(err.r), &error_count] {
        for (std::exception_ptr ep; r >> ep;) ++error_count;
    });
    csp::await_completion();

    CHECK(results == std::vector<std::string>{"42", "7"});
    CHECK(error_count == 1);
}

TEST_CASE("TryMap---error-channel-closed-stops-processing") {
    RunStats stats;
    auto src = chan<int>();
    auto err = chan<std::exception_ptr>();

    auto out = std::move(src.r) | try_map<int>([](int n) -> int {
        if (n < 0) throw std::runtime_error("bad");
        return n;
    }, std::move(err.w));

    // Drop the error reader immediately — error channel is dead.
    err.r = {};

    stats.spawn([w = std::move(src.w)]{
        w << 1;
        w << -1;  // error channel dead → try_map exits
        w << 2;   // never consumed
    });

    std::vector<int> results;
    stats.spawn([r = std::move(out), &results] {
        for (int v; r >> v;) results.push_back(v);
    });
    csp::await_completion();

    CHECK(results == std::vector<int>{1});
}
