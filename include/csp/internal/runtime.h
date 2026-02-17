#pragma once

#include <csp/internal/processor.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace csp::detail {

struct Runtime {
    std::vector<std::unique_ptr<Processor>> procs;  // P0 = main thread
    std::vector<std::thread> workers;               // M1..Mn

    std::mutex global_mu;
    std::deque<Microthread*> global_run_queue;

    std::mutex park_mu;
    std::condition_variable park_cv;

    std::atomic<bool> stopping{false};
    std::atomic<int> live_gs{0};

    // Dynamic processor pool management.
    bool mn_mode_ = false;              // True when num_procs > 1; set once
    std::atomic<int> num_procs_{0};     // Current live P count
    int initial_procs_ = 0;            // P count at init time
    int max_procs_ = 0;                // Upper bound on total Ps
    std::thread watchdog_;              // Heartbeat monitor thread

    static Runtime& instance();
    void init(int num_procs);   // 0 = hardware_concurrency
    void shutdown();
    void unpark_one();

    // Push a microthread to the global run queue.  Caller must
    // hold global_mu.  The microthread must already be delinked
    // from any local queue (next_==nullptr).  The happens-before
    // chain through global_mu guarantees that the next P to pop
    // this MT from the queue will see the null next_/prev_.
    void push_to_global(Microthread* mt);

    void worker_loop();
    void main_loop();
    void watchdog_loop();
    void add_processor();
    Microthread* local_next(Processor& p);
    bool take_from_global(Processor& p);
    void fire_timers(Processor& p);
    bool steal_work(Processor& thief);
    bool has_work(Processor& p);
    std::optional<std::chrono::steady_clock::time_point>
        next_timer_deadline(Processor& p);
};

}
