// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "testutil.h"

#include <stdexcept>
#include <string>
#include <vector>

using namespace csp;

namespace {

struct my_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

}

TEST_CASE("Throw---unbuffered-rethrows-on-read") {
    csp::run([] {
        auto [w, r] = chan<int>{};
        spawn([w = std::move(w)]() mutable {
            w._throw(std::make_exception_ptr(my_error("kaboom")));
        });
        int n = 0;
        bool caught = false;
        try {
            r >> n;
        } catch (my_error const& e) {
            caught = true;
            CHECK(std::string("kaboom") == e.what());
        }
        CHECK(caught);
    });
}

TEST_CASE("Throw---channel-stays-live-after-exception") {
    csp::run([] {
        auto [w, r] = chan<int>{};
        spawn([w = std::move(w)]() mutable {
            w._throw(std::make_exception_ptr(my_error("first")));
            w << 42;
        });
        int n = 0;
        try { r >> n; } catch (my_error const&) {}
        CHECK(static_cast<bool>(r >> n));
        CHECK(42 == n);
    });
}

TEST_CASE("Throw---operator-bool-rethrows") {
    csp::run([] {
        auto [w, r] = chan<int>{};
        spawn([w = std::move(w)]() mutable {
            w._throw(std::make_exception_ptr(my_error("bool-throw")));
        });
        int n = 0;
        bool caught = false;
        try {
            (void)(r >> n);
        } catch (my_error const& e) {
            caught = true;
            CHECK(std::string("bool-throw") == e.what());
        }
        CHECK(caught);
    });
}

TEST_CASE("Throw---prialt-rethrows-on-matched-branch") {
    csp::run([] {
        auto [w1, r1] = chan<int>{};
        auto [w2, r2] = chan<int>{};
        spawn([w1 = std::move(w1)]() mutable {
            w1._throw(std::make_exception_ptr(my_error("via-prialt")));
        });
        // r2 will never fire; r1 wins.
        int a = 0, b = 0;
        bool caught = false;
        try {
            int idx = prialt(r1 >> a, r2 >> b);
            (void)idx;
        } catch (my_error const& e) {
            caught = true;
            CHECK(std::string("via-prialt") == e.what());
        }
        CHECK(caught);
        // r2 unaffected.
        CHECK(static_cast<bool>(r2));
    });
}

TEST_CASE("Throw---multiple-exceptions-in-sequence") {
    csp::run([] {
        auto [w, r] = chan<int>{};
        spawn([w = std::move(w)]() mutable {
            w._throw(std::make_exception_ptr(my_error("e1")));
            w._throw(std::make_exception_ptr(my_error("e2")));
            w << 7;
        });
        int n = 0;
        std::vector<std::string> errs;
        for (int i = 0; i < 2; ++i) {
            try { r >> n; }
            catch (my_error const& e) { errs.push_back(e.what()); }
        }
        REQUIRE(2 == errs.size());
        CHECK("e1" == errs[0]);
        CHECK("e2" == errs[1]);
        CHECK(static_cast<bool>(r >> n));
        CHECK(7 == n);
    });
}

TEST_CASE("Throw---writer-of-exception_ptr-disambiguates") {
    // When T = exception_ptr, `<<` sends an exception_ptr as a *value*
    // (visible at the reader's `r >> ep` slot, NOT thrown), while
    // `_throw` sends an exception_ptr as an exception (rethrown at the
    // reader's call site).
    csp::run([] {
        auto [w, r] = chan<std::exception_ptr>{};
        spawn([w = std::move(w)]() mutable {
            // First: send-as-value.  Reader receives the eptr in its slot.
            w << std::make_exception_ptr(my_error("as-value"));
            // Second: send-as-exception.  Reader rethrows at the read site.
            w._throw(std::make_exception_ptr(my_error("as-exception")));
        });

        // Value path: r >> ep populates ep without throwing.
        std::exception_ptr ep_val;
        bool got = static_cast<bool>(r >> ep_val);
        CHECK(got);
        REQUIRE(ep_val);
        bool nested_ok = false;
        try { std::rethrow_exception(ep_val); }
        catch (my_error const& e) { nested_ok = (std::string("as-value") == e.what()); }
        CHECK(nested_ok);

        // Exception path: r >> ep throws.
        std::exception_ptr ep_unused;
        bool exc_ok = false;
        try { (void)(r >> ep_unused); }
        catch (my_error const& e) { exc_ok = (std::string("as-exception") == e.what()); }
        CHECK(exc_ok);
    });
}
