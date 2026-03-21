// rpc_service.cc — Request/response over channels
//
// A calculator service using CSP's request/response pattern. The server
// reads request<op, double> values and replies via the embedded reply
// channel. Multiple concurrent clients safely share the same request
// endpoint — each call gets its own private reply channel, so responses
// can never be stolen.
//
// Two client styles are shown:
//   w(req)          — blocking: send and wait for the answer
//   call(w, req)    — non-blocking: returns reader<Resp> for later collection

#include "csp.h"

#include <cstdio>
#include <string>

using namespace csp;

struct calc_req {
    char op;
    double a;
    double b;
};

int main() {
    spawn([]{
        printf("RPC calculator service:\n");

        chan<request<calc_req, double>> ch;

        // Server: evaluate arithmetic operations.
        // Spawns per-request so clients can pipeline calls.
        spawn([r = std::move(ch.r)] {
            request<calc_req, double> req;
            while (r >> req) {
                spawn([req = std::move(req)] {
                    auto [op, a, b] = req.value;
                    double result;
                    switch (op) {
                    case '+': result = a + b; break;
                    case '-': result = a - b; break;
                    case '*': result = a * b; break;
                    case '/': result = b != 0 ? a / b : 0; break;
                    default:  result = 0; break;
                    }
                    req.reply << result;
                });
            }
        });

        // Multiple concurrent clients sharing the same writer.
        // Each call creates a private reply channel — no stealing.

        // Client 1: blocking calls via operator()
        spawn([w = ch.w.copy()]() mutable {
            printf("  Client 1: 10 + 32 = %.0f\n", w({'+', 10, 32}));
            printf("  Client 1: 100 / 7 = %.4f\n", w({'/', 100, 7}));
        });

        // Client 2: blocking calls
        spawn([w = ch.w.copy()]() mutable {
            printf("  Client 2: 6 * 7 = %.0f\n", w({'*', 6, 7}));
            printf("  Client 2: 50 - 8 = %.0f\n", w({'-', 50, 8}));
        });

        // Client 3: non-blocking — fire two calls, collect later
        spawn([w = std::move(ch.w)]() mutable {
            auto r1 = call(w, calc_req{'+', 2, 2});
            auto r2 = call(w, calc_req{'*', 3, 3});
            printf("  Client 3: 2 + 2 = %.0f\n", r1.read());
            printf("  Client 3: 3 * 3 = %.0f\n", r2.read());
        });
    });

    schedule();
}
