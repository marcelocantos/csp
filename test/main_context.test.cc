#include "testutil.h"

#include <doctest/doctest.h>

#include <thread>

// 🎯T4.2 — csp dynamic-scope APIs must not crash when called outside
// any imp context (e.g. directly from main(), or from a foreign thread
// that has not been bound to the runtime). Reads gracefully degrade to
// the default; scope/binding operations throw csp::error with a clear
// message.

TEST_SUITE("MainContext") {

TEST_CASE("dynamic-read-returns-default-outside-imp") {
    static csp::dynamic<int> count(99);
    int observed = -1;
    std::thread t([&] {
        // Fresh thread, never bound to any csp Processor.
        observed = *count;
    });
    t.join();
    CHECK(observed == 99);
}

TEST_CASE("dynamic-arrow-returns-default-outside-imp") {
    static csp::dynamic<int> count(7);
    int observed = -1;
    std::thread t([&] {
        observed = *count.operator->();
    });
    t.join();
    CHECK(observed == 7);
}

TEST_CASE("local-throws-clear-error-outside-imp") {
    static csp::dynamic<int> count;
    bool caught = false;
    std::string msg;
    std::thread t([&] {
        try {
            csp::local l{count = 1};
        } catch (const csp::error& e) {
            caught = true;
            msg = e.what();
        }
    });
    t.join();
    CHECK(caught);
    CHECK(msg.find("imp context") != std::string::npos);
}

TEST_CASE("imp_local-read-returns-default-outside-imp") {
    static csp::imp_local<int> v(42);
    int observed = -1;
    std::thread t([&] {
        observed = *v;
    });
    t.join();
    CHECK(observed == 42);
}

TEST_CASE("imp_local-write-throws-clear-error-outside-imp") {
    static csp::imp_local<int> v;
    bool caught = false;
    std::string msg;
    std::thread t([&] {
        try {
            v = 1;
        } catch (const csp::error& e) {
            caught = true;
            msg = e.what();
        }
    });
    t.join();
    CHECK(caught);
    CHECK(msg.find("imp context") != std::string::npos);
}

TEST_CASE("context-current-returns-empty-outside-imp") {
    csp::context ctx;
    std::thread t([&] {
        ctx = csp::context::current();
    });
    t.join();
    CHECK(ctx.root() == 0);
}

TEST_CASE("context_scope-throws-clear-error-outside-imp") {
    csp::context empty;
    bool caught = false;
    std::thread t([&] {
        try {
            csp::context_scope cs(empty);
        } catch (const csp::error& e) {
            caught = true;
        }
    });
    t.join();
    CHECK(caught);
}

} // TEST_SUITE
