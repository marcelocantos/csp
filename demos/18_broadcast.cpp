// 18_broadcast.cpp — One producer, multiple subscribers via share()
//
// share() turns a single reader into a fan-out hub. Each read from the
// returned reader<reader<T>> yields a new subscription channel. Slow
// subscribers see the latest value (backpressure absorbed by the latch imp);
// fast subscribers see every value.
//
// In std C++ broadcasting requires either: a shared mutex protecting a list
// of queues, manual copy-to-every-queue on each write, and careful shutdown
// signalling for each subscriber. With CSP, share() encapsulates all of
// that in a single combinator.

#include "csp.h"

#include <cstdio>

using namespace csp;
using namespace csp::part;

int main() {
    spawn([]{
        auto [src_w, src_r] = chan<int>{};

        // Wrap source reader in a share hub.
        reader<reader<int>> hub = share<int>(std::move(src_r));

        // Collect 3 subscription readers before starting the source.
        reader<int> sub[3];
        for (int i = 0; i < 3; ++i) {
            hub >> sub[i];
        }

        // Spawn 3 subscriber imps.
        for (int i = 0; i < 3; ++i) {
            spawn([i, s = std::move(sub[i])]() mutable {
                for (int v : s) {
                    printf("[sub-%d] %d\n", i + 1, v);
                }
            });
        }

        // Producer sends 1-5 then closes.
        spawn([w = std::move(src_w)]{
            for (int i = 1; i <= 5; ++i) {
                w << i;
            }
        });
    });

    schedule();
}
