// 02_no_mutex.cpp — Concurrent counter without a mutex
//
// Five imps each send 1000 increment messages to a single "server" imp that
// owns the counter. The server serialises all updates — no lock needed because
// only one imp ever touches the counter variable.
//
// In std C++ you'd write:
//   std::mutex mu;
//   { std::lock_guard lk(mu); ++counter; }
// and every thread contends on that lock. Here the channel IS the mutex: sends
// block until the server reads, naturally serialising access with no explicit
// locking and no risk of forgetting to unlock.

#include "csp.h"

#include <cstdio>

using namespace csp;

int main() {
    spawn([]{
        constexpr int WORKERS   = 5;
        constexpr int INCREMENTS = 1000;

        auto [inc_w, inc_r] = chan<int>{};

        // Server imp — sole owner of counter.
        spawn([r = std::move(inc_r)]{
            int counter = 0;
            for (int msg; r >> msg;) {
                counter += msg;
            }
            printf("final counter = %d (expected %d)\n",
                   counter, WORKERS * INCREMENTS);
        });

        // Worker imps — each sends INCREMENTS messages then exits.
        for (int id = 0; id < WORKERS; ++id) {
            spawn([w = inc_w.copy()]{
                for (int i = 0; i < INCREMENTS; ++i) {
                    w << 1;
                }
            });
        }
        inc_w = {};  // Release parent's copy; workers hold the only refs.
    });

    schedule();
}
