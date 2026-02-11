#include "testutil.h"

#include <csp/rpc.h>

using namespace csp;

static Logger g_log("Rpc.Test");

// TODO: test more buffer edge-cases.

TEST_CASE("Rpc - ChanPair") {
    RunStats stats;

    channel<std::tuple<int>> req;
    channel<int> rep;

    spawn(chan::rpc_server(--req, ++rep, [](int n) { return 2 * n + 1; }));

    auto f = chan::rpc_client(++req, --rep);

    CHECK_EQ(1, f(0));
    CHECK_EQ(21, f(10));
    CHECK_EQ(15, f(7));
    CHECK_EQ(-1, f(-1));
}

TEST_CASE("Rpc - VoidReq") {
    RunStats stats;

    channel<std::tuple<>> req;
    channel<int> rep;

    spawn(chan::rpc_server(-req, +rep, []() { return 42; }));

    auto f = chan::rpc_client(+req, -rep);

    CHECK_EQ(42, f());
}

TEST_CASE("Rpc - VoidRep") {
    RunStats stats;

    channel<std::tuple<int>> req;
    channel<> rep;

    int result = 0;

    spawn(chan::rpc_server(-req, +rep, [&result](int n) { result += n; }));

    auto f = chan::rpc_client(+req, -rep);

    for (int n = 1; n <= 10; ++n) {
        f(n);
    }

    CHECK_EQ(55, result);
}

TEST_CASE("Rpc - VoidVoid") {
    RunStats stats;

    channel<std::tuple<>> req;
    channel<> rep;

    int result = 0;

    spawn(chan::rpc_server(-req, +rep, [&result]{ ++result; }));

    auto f = chan::rpc_client(+req, -rep);

    for (int i = 0; i < 10; ++i) {
        f();
    }

    CHECK_EQ(10, result);
}

TEST_CASE("Rpc - RepInReq") {
    RunStats stats;

    channel<std::pair<std::tuple<int>, writer<int>>> req;

    spawn(chan::rpc_server(-req, [](int n) { return 2 * n + 1; }));

    auto f = chan::rpc_client(+req);

    CHECK_EQ(1, f(0));
    CHECK_EQ(21, f(10));
    CHECK_EQ(15, f(7));
    CHECK_EQ(-1, f(-1));
}
