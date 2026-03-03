// 01_ping_pong.cpp — Simplest concurrent program
//
// Two imps bounce a counter back and forth over a pair of channels.
// No mutex, no shared state — each imp owns its own local variables.
// Communication is the synchronisation: sends block until the peer reads.
//
// In std C++ you would need std::thread + std::mutex + std::condition_variable
// (or a promise/future pair) just to pass one value between two threads.
// Here each imp is a cheap coroutine; channels replace all synchronisation
// primitives.

#include "csp.h"

#include <cstdio>

using namespace csp;

int main() {
    spawn([]{
        constexpr int ROUNDS = 5;

        auto [ch1_w, ch1_r] = chan<int>{};
        auto [ch2_w, ch2_r] = chan<int>{};

        // Imp B: receive on ch1, increment, send on ch2.
        spawn([r = std::move(ch1_r), w = std::move(ch2_w)]{
            for (int v; r >> v;) {
                printf("  pong: got %d, sending %d\n", v, v + 1);
                if (!(w << v + 1)) return;
            }
        });

        // Imp A (this imp): send on ch1, receive on ch2.
        int val = 0;
        for (int round = 1; round <= ROUNDS; ++round) {
            printf("ping: sending %d (round %d/%d)\n", val, round, ROUNDS);
            ch1_w << val;
            ch2_r >> val;
        }
        printf("ping: final value = %d\n", val);
    });

    schedule();
}
