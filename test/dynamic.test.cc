#include "testutil.h"

#include <doctest/doctest.h>

TEST_CASE("dynamic: basic read/write") {
    RunStats stats;
    static csp::dynamic<int> count;
    stats.spawn([&]{
        CHECK_EQ(*count, 0);
        count = 42;
        CHECK_EQ(*count, 42);
        count = *count + 1;
        CHECK_EQ(*count, 43);
    });
    csp::schedule();
}

TEST_CASE("dynamic: explicit default") {
    RunStats stats;
    static csp::dynamic<int> count(99);
    stats.spawn([&]{
        CHECK_EQ(*count, 99);
        count = 1;
        CHECK_EQ(*count, 1);
    });
    csp::schedule();
}

TEST_CASE("dynamic: spawn inherits parent context") {
    RunStats stats;
    static csp::dynamic<int> val;
    auto result = csp::chan<int>();
    stats.spawn([&]{
        val = 42;
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
        val = 10;
        stats.spawn([&]{
            val = 99;
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

TEST_CASE("dynamic: context_scope save/restore") {
    RunStats stats;
    static csp::dynamic<int> val;
    stats.spawn([&]{
        val = 1;
        {
            csp::context_scope scope;
            val = 2;
            CHECK_EQ(*val, 2);
        }
        CHECK_EQ(*val, 1);
    });
    csp::schedule();
}

TEST_CASE("dynamic: nested scopes") {
    RunStats stats;
    static csp::dynamic<int> val;
    stats.spawn([&]{
        val = 1;
        {
            csp::context_scope outer;
            val = 2;
            {
                csp::context_scope inner;
                val = 3;
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
        a = 10;
        b = 20;
        CHECK_EQ(*a, 10);
        CHECK_EQ(*b, 20);
        a = 30;
        CHECK_EQ(*a, 30);
        CHECK_EQ(*b, 20);
    });
    csp::schedule();
}

TEST_CASE("dynamic: context transfer over channel") {
    RunStats stats;
    static csp::dynamic<int> val;
    auto ch = csp::chan<csp::context>();
    stats.spawn([&]{
        val = 42;
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
        name = std::string("hello");
        CHECK_EQ(*name, "hello");
    });
    csp::schedule();
}

TEST_CASE("dynamic: deep spawn chain") {
    RunStats stats;
    static csp::dynamic<int> depth;
    auto results = csp::chan<int>();
    stats.spawn([&]{
        depth = 1;
        stats.spawn([&]{
            CHECK_EQ(*depth, 1);
            depth = 2;
            stats.spawn([&]{
                CHECK_EQ(*depth, 2);
                depth = 3;
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
