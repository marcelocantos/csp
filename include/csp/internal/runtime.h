#pragma once

#include <csp/internal/processor.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace csp::detail {

struct Runtime {
    std::vector<std::unique_ptr<Processor>> procs;  // P0 = main thread
    std::vector<std::thread> workers;               // M1..Mn

    std::mutex global_mu;
    std::deque<Imp*> global_run_queue;

    std::mutex park_mu;
    std::condition_variable park_cv;

    std::atomic<bool> stopping{false};
    std::atomic<bool> has_global_work_{false};  // Set by push_to_global, cleared by drain
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

    // Push an imp to the global run queue.  Caller must hold
    // global_mu.  The imp must already be delinked from any local
    // queue (next_==nullptr).  The happens-before chain through
    // global_mu guarantees that the next P to pop this imp from
    // the queue will see the null next_/prev_.
    void push_to_global(Imp* imp);

    void worker_loop();
    void main_loop();
    void watchdog_loop();
    void add_processor();
    Imp* local_next(Processor& p);
    bool take_from_global(Processor& p);
    bool steal_work(Processor& thief);
    bool has_work(Processor& p);
};

}
