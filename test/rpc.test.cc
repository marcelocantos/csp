#include "testutil.h"

using namespace csp;
using namespace csp::part;

static Logger g_log("Rpc.Test");

TEST_CASE("Rpc - ChanPair") {
    RunStats stats;

    auto [req_w, req_r] = chan<std::tuple<int>>{};
    auto [rep_w, rep_r] = chan<int>{};

    spawn(rpc_server(std::move(req_r), std::move(rep_w), [](int n) { return 2 * n + 1; }));

    auto f = rpc_client(std::move(req_w), std::move(rep_r));

    CHECK(1 == f(0));
    CHECK(21 == f(10));
    CHECK(15 == f(7));
    CHECK(-1 == f(-1));
}

TEST_CASE("Rpc - VoidReq") {
    RunStats stats;

    auto [req_w, req_r] = chan<std::tuple<>>{};
    auto [rep_w, rep_r] = chan<int>{};

    spawn(rpc_server(req_r.copy(), rep_w.copy(), []() { return 42; }));

    auto f = rpc_client(req_w.copy(), rep_r.copy());

    CHECK(42 == f());
}

TEST_CASE("Rpc - VoidRep") {
    RunStats stats;

    auto [req_w, req_r] = chan<std::tuple<int>>{};
    auto [rep_w, rep_r] = chan<>{};

    int result = 0;

    spawn(rpc_server(req_r.copy(), rep_w.copy(), [&result](int n) { result += n; }));

    auto f = rpc_client(req_w.copy(), rep_r.copy());

    for (int n = 1; n <= 10; ++n) {
        f(n);
    }

    CHECK(55 == result);
}

TEST_CASE("Rpc - VoidVoid") {
    RunStats stats;

    auto [req_w, req_r] = chan<std::tuple<>>{};
    auto [rep_w, rep_r] = chan<>{};

    int result = 0;

    spawn(rpc_server(req_r.copy(), rep_w.copy(), [&result]{ ++result; }));

    auto f = rpc_client(req_w.copy(), rep_r.copy());

    for (int i = 0; i < 10; ++i) {
        f();
    }

    CHECK(10 == result);
}

TEST_CASE("Rpc - RepInReq") {
    RunStats stats;

    auto [req_w, req_r] = chan<std::pair<std::tuple<int>, writer<int>>>{};

    spawn(rpc_server(req_r.copy(), [](int n) { return 2 * n + 1; }));

    auto f = rpc_client(req_w.copy());

    CHECK(1 == f(0));
    CHECK(21 == f(10));
    CHECK(15 == f(7));
    CHECK(-1 == f(-1));
}
