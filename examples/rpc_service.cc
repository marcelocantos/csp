// rpc_service.cc — Request/response over channels
//
// A calculator service using CSP's RPC pattern. The server accepts
// (op, a, b) tuples on a request channel, applies the operation,
// and returns the result on a reply channel. Multiple concurrent
// clients share the same server.
//
// The chan::rpc_client / chan::rpc_server combinators abstract away
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
        channel<std::tuple<char, double, double>> req;
        channel<double> rep;

        // Server: evaluate arithmetic operations
        spawn(chan::rpc_server<char, double, double>(
            --req, ++rep,
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

        // Create the client callable. Each copy holds refs to req/rep,
        // keeping the server alive as long as any client exists.
        auto calc = chan::rpc_client<char, double, double>(++req, --rep);

        // Multiple concurrent clients
        spawn([calc]{
            printf("  Client 1: 10 + 32 = %.0f\n", calc('+', 10, 32));
            printf("  Client 1: 100 / 7 = %.4f\n", calc('/', 100, 7));
        });

        spawn([calc]{
            printf("  Client 2: 6 * 7 = %.0f\n", calc('*', 6, 7));
            printf("  Client 2: 50 - 8 = %.0f\n", calc('-', 50, 8));
        });

        spawn([calc]{
            printf("  Client 3: 2 + 2 = %.0f\n", calc('+', 2, 2));
        });
    });

    schedule();
}
