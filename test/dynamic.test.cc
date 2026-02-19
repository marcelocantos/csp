#include "testutil.h"

#include <doctest/doctest.h>

TEST_CASE("dynamic: basic read/write") {
    RunStats stats;
    static csp::dynamic<int> count;
    stats.spawn([&]{
        CHECK_EQ(*count, 0);
        csp::local l{count = 42};
        CHECK_EQ(*count, 42);
        {
            csp::local l2{count = *count + 1};
            CHECK_EQ(*count, 43);
        }
        CHECK_EQ(*count, 42);
    });
    csp::schedule();
}

TEST_CASE("dynamic: explicit default") {
    RunStats stats;
    static csp::dynamic<int> count(99);
    stats.spawn([&]{
        CHECK_EQ(*count, 99);
        csp::local l{count = 1};
        CHECK_EQ(*count, 1);
    });
    csp::schedule();
}

TEST_CASE("dynamic: spawn inherits parent context") {
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
        CHECK_EQ(v, 42);
    });
    csp::schedule();
}

TEST_CASE("dynamic: child write isolation") {
    RunStats stats;
    static csp::dynamic<int> val;
    auto child_done = csp::chan<int>();
    auto parent_result = csp::chan<int>();
    stats.spawn([&]{
        csp::local l{val = 10};
        stats.spawn([&]{
            csp::local l2{val = 99};
            CHECK_EQ(*val, 99);
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
        CHECK_EQ(v, 10);
    });
    csp::schedule();
}

TEST_CASE("dynamic: local scoped revert") {
    RunStats stats;
    static csp::dynamic<int> val;
    stats.spawn([&]{
        csp::local l{val = 1};
        {
            csp::local l2{val = 2};
            CHECK_EQ(*val, 2);
        }
        CHECK_EQ(*val, 1);
    });
    csp::schedule();
}

TEST_CASE("dynamic: nested locals") {
    RunStats stats;
    static csp::dynamic<int> val;
    stats.spawn([&]{
        csp::local l1{val = 1};
        {
            csp::local l2{val = 2};
            {
                csp::local l3{val = 3};
                CHECK_EQ(*val, 3);
            }
            CHECK_EQ(*val, 2);
        }
        CHECK_EQ(*val, 1);
    });
    csp::schedule();
}

TEST_CASE("dynamic: multiple keys") {
    RunStats stats;
    static csp::dynamic<int> a;
    static csp::dynamic<int> b;
    stats.spawn([&]{
        csp::local l{a = 10, b = 20};
        CHECK_EQ(*a, 10);
        CHECK_EQ(*b, 20);
        {
            csp::local l2{a = 30};
            CHECK_EQ(*a, 30);
            CHECK_EQ(*b, 20);
        }
        CHECK_EQ(*a, 10);
    });
    csp::schedule();
}

TEST_CASE("dynamic: context transfer over channel") {
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
        CHECK_EQ(*val, 0);  // default before injection
        {
            csp::context_scope scope(ctx);
            CHECK_EQ(*val, 42);  // injected context
        }
        CHECK_EQ(*val, 0);  // restored
    });
    csp::schedule();
}

TEST_CASE("dynamic: string type") {
    RunStats stats;
    static csp::dynamic<std::string> name("default");
    stats.spawn([&]{
        CHECK_EQ(*name, "default");
        csp::local l{name = std::string("hello")};
        CHECK_EQ(*name, "hello");
    });
    csp::schedule();
}

TEST_CASE("dynamic: deep spawn chain") {
    RunStats stats;
    static csp::dynamic<int> depth;
    auto results = csp::chan<int>();
    stats.spawn([&]{
        csp::local l1{depth = 1};
        stats.spawn([&]{
            CHECK_EQ(*depth, 1);
            csp::local l2{depth = 2};
            stats.spawn([&]{
                CHECK_EQ(*depth, 2);
                csp::local l3{depth = 3};
                CHECK_EQ(*depth, 3);
                results.w << *depth;
            });
        });
    });
    stats.spawn([&]{
        int v;
        results.r >> v;
        CHECK_EQ(v, 3);
    });
    csp::schedule();
}

TEST_CASE("dynamic: multi-bind local") {
    RunStats stats;
    static csp::dynamic<int> x;
    static csp::dynamic<int> y;
    static csp::dynamic<std::string> z("none");
    stats.spawn([&]{
        csp::local l{x = 1, y = 2, z = std::string("hello")};
        CHECK_EQ(*x, 1);
        CHECK_EQ(*y, 2);
        CHECK_EQ(*z, "hello");
    });
    csp::schedule();
}

// --- mt_local tests ---

TEST_CASE("mt_local: basic read/write") {
    RunStats stats;
    static csp::mt_local<int> counter;
    stats.spawn([&]{
        CHECK_EQ(*counter, 0);
        counter = 42;
        CHECK_EQ(*counter, 42);
        counter = *counter + 1;
        CHECK_EQ(*counter, 43);
    });
    csp::schedule();
}

TEST_CASE("mt_local: explicit default") {
    RunStats stats;
    static csp::mt_local<int> counter(99);
    stats.spawn([&]{
        CHECK_EQ(*counter, 99);
        counter = 1;
        CHECK_EQ(*counter, 1);
    });
    csp::schedule();
}

TEST_CASE("mt_local: not inherited by child") {
    RunStats stats;
    static csp::mt_local<int> val;
    auto result = csp::chan<int>();
    stats.spawn([&]{
        val = 42;
        CHECK_EQ(*val, 42);
        stats.spawn([&]{
            // Child should NOT see parent's value.
            result.w << *val;
        });
    });
    stats.spawn([&]{
        int v;
        result.r >> v;
        CHECK_EQ(v, 0);  // default, not 42
    });
    csp::schedule();
}

TEST_CASE("mt_local: independent per microthread") {
    RunStats stats;
    static csp::mt_local<int> val;
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
        CHECK_EQ(v1, 10);
        CHECK_EQ(v2, 20);
    });
    csp::schedule();
}

TEST_CASE("mt_local: string type") {
    RunStats stats;
    static csp::mt_local<std::string> name("default");
    stats.spawn([&]{
        CHECK_EQ(*name, "default");
        name = std::string("hello");
        CHECK_EQ(*name, "hello");
    });
    csp::schedule();
}

