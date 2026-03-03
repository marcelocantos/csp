// 13_backpressure.cpp — Natural flow control via synchronous channels
//
// A fast producer and a slow consumer share an unbuffered channel. Because
// sends block until the peer reads, the producer is automatically throttled
// to the consumer's pace — no ringbuffer overflow, no dropped messages, no OOM.
//
// In std C++ you'd need a bounded queue + condition variables to achieve the
// same effect. With synchronous channels, backpressure is built in: the channel
// IS the handshake.

#include "csp.h"

#include <cstdio>
#include <chrono>

using namespace csp;
using namespace std::chrono;
using namespace std::chrono_literals;

int main() {
    spawn([]{
        constexpr int N = 8;
        auto t0 = steady_clock::now();

        auto [w, r] = chan<int>{};

        spawn([w = std::move(w), t0]{
            for (int i = 0; i < N; ++i) {
                // Record time just before blocking send.
                auto before = duration_cast<milliseconds>(steady_clock::now() - t0).count();
                if (!(w << i)) break;
                printf("producer sent %d at %lldms\n", i, (long long)before);
            }
        });

        // Slow consumer — 50ms between reads.
        for (int v; r >> v;) {
            sleep(50ms);
        }
    });

    schedule();
}
