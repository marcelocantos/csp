// 14_graceful_shutdown.cpp — Shutdown propagation via channel death
//
// A 3-stage pipeline: source → transform → sink. When source finishes and
// its writer goes out of scope, the transform's reader exhausts and the
// transform exits. Its writer then closes, exhausting the sink's reader.
// Shutdown propagates naturally through the pipeline — no stop_source,
// no atomic<bool> running, no "please stop" protocol.
//
// In std C++ pipelines typically require a separate stop signal threaded
// through every stage. Here, closing a channel IS the stop signal.

#include "csp.h"

#include <cstdio>

using namespace csp;

int main() {
    spawn([]{
        auto [w1, r1] = chan<int>{};
        auto [w2, r2] = chan<int>{};

        // Source: sends 5 values then exits; writer closes on scope exit.
        spawn([w = std::move(w1)]{
            printf("source: starting\n");
            for (int i = 1; i <= 5; ++i) {
                w << i;
            }
            printf("source: done, closing channel\n");
        });

        // Transform: doubles values; exits when source channel exhausts.
        spawn([r = std::move(r1), w = std::move(w2)]{
            printf("transform: starting\n");
            for (int v; r >> v;) {
                w << v * 2;
            }
            printf("transform: source exhausted, closing channel\n");
        });

        // Sink: prints values; exits when transform channel exhausts.
        spawn([r = std::move(r2)]{
            printf("sink: starting\n");
            for (int v : r) {
                printf("sink: received %d\n", v);
            }
            printf("sink: transform exhausted, done\n");
        });
    });

    schedule();
}
