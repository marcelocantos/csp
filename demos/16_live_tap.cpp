// 16_live_tap.cpp — Non-invasive pipeline observation with tap()
//
// Add a side observer to a live channel without modifying the producer or
// consumer. tap() returns an observer reader that receives every value the
// main reader receives — both must consume for the producer to proceed.
//
// In std C++ adding observability to an existing pipeline requires plumbing
// through extra queues or wrapping the producer with a broadcast helper.
// With CSP, tap() intercepts values in-place: producer and consumer code
// stay unchanged, and the observer is just another reader.

#include "csp.h"

#include <cstdio>

using namespace csp;

int main() {
    spawn([]{
        auto ch = chan<int>{};

        // Attach observer. Both ch.r and observer must be read for producer
        // to unblock. tap() inserts itself transparently.
        auto observer = tap(ch.w, ch.r);

        // Producer — knows nothing about the observer.
        spawn([w = std::move(ch.w)]{
            for (int i = 1; i <= 5; ++i) {
                w << i;
            }
        });

        // Observer imp — reads from the tap side channel.
        spawn([obs = std::move(observer)]{
            for (int v : obs) {
                printf("[tap] %d\n", v);
            }
        });

        // Main consumer — reads from the original reader.
        for (int v : ch.r) {
            printf("[main] %d\n", v);
        }
    });

    schedule();
}
