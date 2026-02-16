#include "testutil.h"

#include <csp/rpc.h>

using namespace csp;

static Logger g_log("Rpc.Test");

// TODO: test more buffer edge-cases.

TEST_CASE("Rpc - ChanPair") {
    RunStats stats;

    chan<std::tuple<int>> req;
    chan<int> rep;

    spawn(rpc_server(std::move(req.r), std::move(rep.w), [](int n) { return 2 * n + 1; }));

    auto f = rpc_client(std::move(req.w), std::move(rep.r));

    CHECK_EQ(1, f(0));
    CHECK_EQ(21, f(10));
    CHECK_EQ(15, f(7));
    CHECK_EQ(-1, f(-1));
}

TEST_CASE("Rpc - VoidReq") {
    RunStats stats;

    chan<std::tuple<>> req;
    chan<int> rep;

    spawn(rpc_server(req.r.copy(), rep.w.copy(), []() { return 42; }));

    auto f = rpc_client(req.w.copy(), rep.r.copy());

    CHECK_EQ(42, f());
}

TEST_CASE("Rpc - VoidRep") {
    RunStats stats;

    chan<std::tuple<int>> req;
    chan<> rep;

    int result = 0;

    spawn(rpc_server(req.r.copy(), rep.w.copy(), [&result](int n) { result += n; }));

    auto f = rpc_client(req.w.copy(), rep.r.copy());

    for (int n = 1; n <= 10; ++n) {
        f(n);
    }

    CHECK_EQ(55, result);
}

TEST_CASE("Rpc - VoidVoid") {
    RunStats stats;

    chan<std::tuple<>> req;
    chan<> rep;

    int result = 0;

    spawn(rpc_server(req.r.copy(), rep.w.copy(), [&result]{ ++result; }));

    auto f = rpc_client(req.w.copy(), rep.r.copy());

    for (int i = 0; i < 10; ++i) {
        f();
    }

    CHECK_EQ(10, result);
}

TEST_CASE("Rpc - RepInReq") {
    RunStats stats;

    chan<std::pair<std::tuple<int>, writer<int>>> req;

    spawn(rpc_server(req.r.copy(), [](int n) { return 2 * n + 1; }));

    auto f = rpc_client(req.w.copy());

    CHECK_EQ(1, f(0));
    CHECK_EQ(21, f(10));
    CHECK_EQ(15, f(7));
    CHECK_EQ(-1, f(-1));
}
