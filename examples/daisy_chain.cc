// daisy_chain.cc — 100,000 microthreads
//
// Create a chain of N microthreads, each connected to the next by a
// channel. Send a value in one end; each microthread increments it
// and forwards to the next. Receive the final value at the other end.
//
// This demonstrates that CSP microthreads are ultra-lightweight —
// 100,000 concurrent contexts where OS threads would exhaust memory.
// (An OS thread typically needs 1-8 MB of stack; a CSP microthread
// uses a few hundred bytes.)

#include "csp.h"

#include <cstdio>
#include <chrono>

using namespace csp;

int main() {
    spawn([]{
        constexpr int N = 100000;

        auto start = std::chrono::steady_clock::now();

        // Build the chain right-to-left.
        chan<int> rightmost;
        reader<int> right = rightmost.r.copy();

        for (int i = 0; i < N; ++i) {
            chan<int> left;
            spawn([in = left.r.copy(), out = rightmost.w.copy()]{
                int n;
                if (in >> n) {
                    out << (n + 1);
                }
            });
            rightmost = std::move(left);
        }

        auto built = std::chrono::steady_clock::now();

        // Send 0 into the left end.
        rightmost.w << 0;

        // Read from the right end.
        int result = right.read();

        auto done = std::chrono::steady_clock::now();

        auto build_us = std::chrono::duration_cast<std::chrono::microseconds>(built - start).count();
        auto run_us = std::chrono::duration_cast<std::chrono::microseconds>(done - built).count();

        printf("Daisy chain: %d microthreads\n", N);
        printf("  Result: %d (expected %d)\n", result, N);
        printf("  Build time: %lld us (%.1f us/thread)\n", build_us, (double)build_us / N);
        printf("  Run time:   %lld us (%.1f us/hop)\n", run_us, (double)run_us / N);
    });

    schedule();
}
