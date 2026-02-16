// rpc_service.cc — Request/response over channels
//
// A calculator service using CSP's RPC pattern. The server accepts
// (op, a, b) tuples on a request channel, applies the operation,
// and returns the result on a reply channel. Multiple concurrent
// clients share the same server.
//
// The rpc_client / rpc_server combinators abstract away
// the request/response plumbing. Compare to the boilerplate of
// futures + promises, or callback-based async patterns.

#include <csp/microthread.h>
#include <csp/rpc.h>

#include <cstdio>
#include <tuple>

using namespace csp;

int main() {
    spawn([]{
        printf("RPC calculator service:\n");

        // RPC channel pair
        auto [req_w, req_r] = chan<std::tuple<char, double, double>>{};
        auto [rep_w, rep_r] = chan<double>{};

        // Server: evaluate arithmetic operations
        spawn(rpc_server<char, double, double>(
            std::move(req_r), std::move(rep_w),
            [](char op, double a, double b) -> double {
                switch (op) {
                case '+': return a + b;
                case '-': return a - b;
                case '*': return a * b;
                case '/': return b != 0 ? a / b : 0;
                default:  return 0;
                }
            }
        ));

        // Each client needs its own rpc_client (move-only endpoints).
        auto calc1 = rpc_client<char, double, double>(req_w.copy(), rep_r.copy());
        auto calc2 = rpc_client<char, double, double>(req_w.copy(), rep_r.copy());
        auto calc3 = rpc_client<char, double, double>(std::move(req_w), std::move(rep_r));

        // Multiple concurrent clients
        spawn([calc = std::move(calc1)]() mutable {
            printf("  Client 1: 10 + 32 = %.0f\n", calc('+', 10, 32));
            printf("  Client 1: 100 / 7 = %.4f\n", calc('/', 100, 7));
        });

        spawn([calc = std::move(calc2)]() mutable {
            printf("  Client 2: 6 * 7 = %.0f\n", calc('*', 6, 7));
            printf("  Client 2: 50 - 8 = %.0f\n", calc('-', 50, 8));
        });

        spawn([calc = std::move(calc3)]() mutable {
            printf("  Client 3: 2 + 2 = %.0f\n", calc('+', 2, 2));
        });
    });

    schedule();
}
