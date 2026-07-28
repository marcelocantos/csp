#include "testutil.h"

#include <doctest/doctest.h>

#include <stdexcept>
#include <string>

TEST_CASE("dynamic:-basic-read/write") {
    RunStats stats;
    static csp::dynamic<int> count;
    stats.spawn([&]{
        CHECK(*count == 0);
        csp::local l{count = 42};
        CHECK(*count == 42);
        {
            csp::local l2{count = *count + 1};
            CHECK(*count == 43);
        }
        CHECK(*count == 42);
    });
    csp::await_completion();
}

TEST_CASE("dynamic:-explicit-default") {
    RunStats stats;
    static csp::dynamic<int> count(99);
    stats.spawn([&]{
        CHECK(*count == 99);
        csp::local l{count = 1};
        CHECK(*count == 1);
    });
    csp::await_completion();
}

TEST_CASE("dynamic:-spawn-inherits-parent-context") {
    RunStats stats;
    static csp::dynamic<int> val;
    auto result = csp::chan<int>();
    stats.spawn([&]{
        csp::local l{val = 42};
        stats.spawn([&]{
            result.w << *val;
        });
    });
    stats.spawn([&]{
        int v;
        result.r >> v;
        CHECK(v == 42);
    });
    csp::await_completion();
}

TEST_CASE("dynamic:-child-write-isolation") {
    RunStats stats;
    static csp::dynamic<int> val;
    auto child_done = csp::chan<int>();
    auto parent_result = csp::chan<int>();
    stats.spawn([&]{
        csp::local l{val = 10};
        stats.spawn([&]{
            csp::local l2{val = 99};
            CHECK(*val == 99);
            child_done.w << 0;
        });
        int dummy;
        child_done.r >> dummy;
        // Parent's context should be unaffected by child write.
        parent_result.w << *val;
    });
    stats.spawn([&]{
        int v;
        parent_result.r >> v;
        CHECK(v == 10);
    });
    csp::await_completion();
}

TEST_CASE("dynamic:-local-scoped-revert") {
    RunStats stats;
    static csp::dynamic<int> val;
    stats.spawn([&]{
        csp::local l{val = 1};
        {
            csp::local l2{val = 2};
            CHECK(*val == 2);
        }
        CHECK(*val == 1);
    });
    csp::await_completion();
}

TEST_CASE("dynamic:-nested-locals") {
    RunStats stats;
    static csp::dynamic<int> val;
    stats.spawn([&]{
        csp::local l1{val = 1};
        {
            csp::local l2{val = 2};
            {
                csp::local l3{val = 3};
                CHECK(*val == 3);
            }
            CHECK(*val == 2);
        }
        CHECK(*val == 1);
    });
    csp::await_completion();
}

TEST_CASE("dynamic:-multiple-keys") {
    RunStats stats;
    static csp::dynamic<int> a;
    static csp::dynamic<int> b;
    stats.spawn([&]{
        csp::local l{a = 10, b = 20};
        CHECK(*a == 10);
        CHECK(*b == 20);
        {
            csp::local l2{a = 30};
            CHECK(*a == 30);
            CHECK(*b == 20);
        }
        CHECK(*a == 10);
    });
    csp::await_completion();
}

TEST_CASE("dynamic:-context-transfer-over-channel") {
    RunStats stats;
    static csp::dynamic<int> val;
    auto ch = csp::chan<csp::context>();
    stats.spawn([&]{
        csp::local l{val = 42};
        ch.w << csp::context::current();
    });
    stats.spawn([&]{
        csp::context ctx;
        ch.r >> ctx;
        CHECK(*val == 0);  // default before injection
        {
            csp::context_scope scope(ctx);
            CHECK(*val == 42);  // injected context
        }
        CHECK(*val == 0);  // restored
    });
    csp::await_completion();
}

TEST_CASE("dynamic:-string-type") {
    RunStats stats;
    static csp::dynamic<std::string> name("default");
    stats.spawn([&]{
        CHECK(*name == "default");
        csp::local l{name = std::string("hello")};
        CHECK(*name == "hello");
    });
    csp::await_completion();
}

TEST_CASE("dynamic:-deep-spawn-chain") {
    RunStats stats;
    static csp::dynamic<int> depth;
    auto results = csp::chan<int>();
    stats.spawn([&]{
        csp::local l1{depth = 1};
        stats.spawn([&]{
            CHECK(*depth == 1);
            csp::local l2{depth = 2};
            stats.spawn([&]{
                CHECK(*depth == 2);
                csp::local l3{depth = 3};
                CHECK(*depth == 3);
                results.w << *depth;
            });
        });
    });
    stats.spawn([&]{
        int v;
        results.r >> v;
        CHECK(v == 3);
    });
    csp::await_completion();
}

TEST_CASE("dynamic:-multi-bind-local") {
    RunStats stats;
    static csp::dynamic<int> x;
    static csp::dynamic<int> y;
    static csp::dynamic<std::string> z("none");
    stats.spawn([&]{
        csp::local l{x = 1, y = 2, z = std::string("hello")};
        CHECK(*x == 1);
        CHECK(*y == 2);
        CHECK(*z == "hello");
    });
    csp::await_completion();
}

TEST_CASE("dynamic:-local-binding-reverts-during-exception") {
    RunStats stats;
    static csp::dynamic<int> val(0);
    int after_catch = -1;
    stats.spawn([&]{
        csp::local l{val = 10};
        try {
            csp::local l2{val = 99};
            CHECK(*val == 99);
            throw std::runtime_error("test exception");
        } catch (std::runtime_error const&) {
            after_catch = *val;
        }
    });
    csp::await_completion();
    CHECK(after_catch == 10);
}

TEST_CASE("dynamic:-multiple-locals-in-same-scope-both-revert") {
    RunStats stats;
    static csp::dynamic<int> a(0);
    static csp::dynamic<std::string> b("default");
    int a_after = -1;
    std::string b_after;
    stats.spawn([&]{
        {
            csp::local l{a = 42, b = std::string("hello")};
            CHECK(*a == 42);
            CHECK(*b == "hello");
        }
        a_after = *a;
        b_after = *b;
    });
    csp::await_completion();
    CHECK(a_after == 0);
    CHECK(b_after == "default");
}

// --- imp_local tests ---

TEST_CASE("imp_local:-basic-read/write") {
    RunStats stats;
    static csp::imp_local<int> counter;
    stats.spawn([&]{
        CHECK(*counter == 0);
        counter = 42;
        CHECK(*counter == 42);
        counter = *counter + 1;
        CHECK(*counter == 43);
    });
    csp::await_completion();
}

TEST_CASE("imp_local:-explicit-default") {
    RunStats stats;
    static csp::imp_local<int> counter(99);
    stats.spawn([&]{
        CHECK(*counter == 99);
        counter = 1;
        CHECK(*counter == 1);
    });
    csp::await_completion();
}

TEST_CASE("imp_local:-not-inherited-by-child") {
    RunStats stats;
    static csp::imp_local<int> val;
    auto result = csp::chan<int>();
    stats.spawn([&]{
        val = 42;
        CHECK(*val == 42);
        stats.spawn([&]{
            // Child should NOT see parent's value.
            result.w << *val;
        });
    });
    stats.spawn([&]{
        int v;
        result.r >> v;
        CHECK(v == 0);  // default, not 42
    });
    csp::await_completion();
}

TEST_CASE("imp_local:-independent-per-imp") {
    RunStats stats;
    static csp::imp_local<int> val;
    auto ch1 = csp::chan<int>();
    auto ch2 = csp::chan<int>();
    stats.spawn([&]{
        val = 10;
        ch1.w << *val;
    });
    stats.spawn([&]{
        val = 20;
        ch2.w << *val;
    });
    stats.spawn([&]{
        int v1, v2;
        ch1.r >> v1;
        ch2.r >> v2;
        CHECK(v1 == 10);
        CHECK(v2 == 20);
    });
    csp::await_completion();
}

TEST_CASE("imp_local:-string-type") {
    RunStats stats;
    static csp::imp_local<std::string> name("default");
    stats.spawn([&]{
        CHECK(*name == "default");
        name = std::string("hello");
        CHECK(*name == "hello");
    });
    csp::await_completion();
}

