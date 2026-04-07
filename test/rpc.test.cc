#include "testutil.h"

using namespace csp;
using namespace csp::part;

static Logger g_log("Rpc.Test");

TEST_CASE("Rpc---ChanPair") {
    RunStats stats;

    csp::run([&] {
        auto [req_w, req_r] = chan<std::tuple<int>>{};
        auto [rep_w, rep_r] = chan<int>{};

        spawn(rpc_server(std::move(req_r), std::move(rep_w), [](int n) { return 2 * n + 1; }));

        auto f = rpc_client(std::move(req_w), std::move(rep_r));

        CHECK(1 == f(0));
        CHECK(21 == f(10));
        CHECK(15 == f(7));
        CHECK(-1 == f(-1));
    });
}

TEST_CASE("Rpc---VoidReq") {
    RunStats stats;

    csp::run([&] {
        auto [req_w, req_r] = chan<std::tuple<>>{};
        auto [rep_w, rep_r] = chan<int>{};

        spawn(rpc_server(req_r.copy(), rep_w.copy(), []() { return 42; }));

        auto f = rpc_client(req_w.copy(), rep_r.copy());

        CHECK(42 == f());
    });
}

TEST_CASE("Rpc---VoidRep") {
    RunStats stats;

    int result = 0;

    csp::run([&] {
        auto [req_w, req_r] = chan<std::tuple<int>>{};
        auto [rep_w, rep_r] = chan<>{};

        spawn(rpc_server(req_r.copy(), rep_w.copy(), [&result](int n) { result += n; }));

        auto f = rpc_client(req_w.copy(), rep_r.copy());

        for (int n = 1; n <= 10; ++n) {
            f(n);
        }
    });

    CHECK(55 == result);
}

TEST_CASE("Rpc---VoidVoid") {
    RunStats stats;

    int result = 0;

    csp::run([&] {
        auto [req_w, req_r] = chan<std::tuple<>>{};
        auto [rep_w, rep_r] = chan<>{};

        spawn(rpc_server(req_r.copy(), rep_w.copy(), [&result]{ ++result; }));

        auto f = rpc_client(req_w.copy(), rep_r.copy());

        for (int i = 0; i < 10; ++i) {
            f();
        }
    });

    CHECK(10 == result);
}

TEST_CASE("Rpc---RepInReq") {
    RunStats stats;

    csp::run([&] {
        auto [req_w, req_r] = chan<std::pair<std::tuple<int>, writer<int>>>{};

        spawn(rpc_server(req_r.copy(), [](int n) { return 2 * n + 1; }));

        auto f = rpc_client(req_w.copy());

        CHECK(1 == f(0));
        CHECK(21 == f(10));
        CHECK(15 == f(7));
        CHECK(-1 == f(-1));
    });
}

TEST_CASE("request/call---basic") {
    RunStats stats;

    csp::run([&] {
        chan<request<int, int>> ch;

        // Server: reads requests, replies with 2*n+1.
        spawn([r = std::move(ch.r)] {
            request<int, int> req;
            while (r >> req) {
                req.reply << 2 * req.value + 1;
            }
        });

        CHECK(1 == call(ch.w, 0).read());
        CHECK(21 == call(ch.w, 10).read());
        CHECK(15 == call(ch.w, 7).read());
    });
}

TEST_CASE("request/operator()---blocking-call") {
    RunStats stats;

    csp::run([&] {
        chan<request<int, int>> ch;

        spawn([r = std::move(ch.r)] {
            request<int, int> req;
            while (r >> req) {
                req.reply << req.value * req.value;
            }
        });

        // writer::operator() sends and blocks for response.
        CHECK(0 == ch.w(0));
        CHECK(9 == ch.w(3));
        CHECK(49 == ch.w(7));
        CHECK(100 == ch.w(10));
    });
}

TEST_CASE("request/rpc---concurrent-callers") {
    RunStats stats;

    csp::run([&] {
        chan<request<int, int>> ch;

        // Server: handles requests concurrently by spawning per-request.
        spawn([r = std::move(ch.r)] {
            request<int, int> req;
            while (r >> req) {
                spawn([req = std::move(req)] {
                    req.reply << req.value * req.value;
                });
            }
        });

        // Fire off multiple RPCs before reading any responses.
        auto r1 = call(ch.w, 3);
        auto r2 = call(ch.w, 7);
        auto r3 = call(ch.w, 11);

        CHECK(9 == r1.read());
        CHECK(49 == r2.read());
        CHECK(121 == r3.read());
    });
}

TEST_CASE("request/rpc---server-death") {
    RunStats stats;

    csp::run([&] {
        chan<request<int, int>> ch;

        // Server that processes one request then dies.
        spawn([r = std::move(ch.r)] {
            request<int, int> req;
            if (r >> req) {
                req.reply << req.value + 1;
            }
        });

        CHECK(6 == call(ch.w, 5).read());

        // Server is dead — next rpc's write fails (chan_op destructor
        // blocks in prialt, sees dead channel).
        CHECK_FALSE(bool(ch.w << request<int, int>{99, {}}));
    });
}
