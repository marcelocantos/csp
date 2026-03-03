// 08_first_response.cpp — Race N backends, take the first result
//
// Spawn 4 "backend" imps with different latencies. All write to a
// shared channel. Read once — that's the winner. Drop the reader —
// the losers' writes silently fail and they exit. No cancellation
// tokens, no promise/future, no careful lifetime management.

#include "csp.h"

#include <chrono>
#include <cstdio>

using namespace csp;
using namespace std::chrono_literals;

int main() {
    spawn([]{
        auto [w, r] = chan<int>{};

        int latencies[] = {40, 10, 30, 20};  // ms
        for (int i = 0; i < 4; ++i) {
            spawn([w = w.copy(), i, d = latencies[i]]{
                csp::sleep(std::chrono::milliseconds(d));
                w << i;  // may fail if reader already gone — that's fine
            });
        }
        w = {};  // release our copy; backends hold the refs

        int winner = r.read();
        printf("  backend %d won (latency %dms)\n", winner, latencies[winner]);
        // r dropped here → remaining backends' writes fail → they exit
    });

    schedule();
}
