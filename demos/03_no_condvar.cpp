// 03_no_condvar.cpp — Bounded producer/consumer without condition variables
//
// A buffered channel chan<int>(4) acts as a bounded queue. The producer
// blocks automatically when the buffer is full; the consumer blocks when it
// is empty. No mutex, no condition_variable, no spurious wakeup handling.
//
// In std C++ a bounded queue requires:
//   std::mutex mu;
//   std::condition_variable not_full, not_empty;
//   while (queue.size() == CAP) not_full.wait(lk);   // spurious wakeup loop
//   while (queue.empty())       not_empty.wait(lk);  // spurious wakeup loop
//
// Here the channel handles all of that — the producer just writes and the
// channel blocks it when the buffer is full.

#include "csp.h"

#include <cstdio>
#include <chrono>

using namespace csp;
using namespace std::chrono_literals;

int main() {
    spawn([]{
        constexpr int ITEMS      = 20;
        constexpr int BUF_SIZE   =  4;

        auto [w, r] = chan<int>(BUF_SIZE);

        // Producer — sends ITEMS values.
        spawn([w = std::move(w)]{
            for (int i = 0; i < ITEMS; ++i) {
                printf("produce %2d\n", i);
                w << i;
            }
            printf("producer done\n");
        });

        // Consumer — reads slowly to demonstrate backpressure.
        int count = 0;
        for (int v; r >> v;) {
            printf("  consume %2d\n", v);
            csp::sleep(10ms);
            if (++count >= ITEMS) break;
        }
        printf("consumer done (%d items)\n", count);
    });

    schedule();
}
