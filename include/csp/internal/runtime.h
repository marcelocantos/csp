#pragma once

#include <csp/internal/processor.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace csp::detail {

struct Runtime {
    ~Runtime() { shutdown(); }

    std::vector<std::unique_ptr<Processor>> procs;  // P0 = main thread

    std::mutex global_mu;
    std::deque<Imp*> global_run_queue;

    std::mutex park_mu;
    std::condition_variable park_cv;

    std::atomic<bool> stopping{false};
    std::atomic<bool> has_global_work_{false};  // Set by push_to_global, cleared by drain
    std::atomic<int> live_gs{0};
    std::atomic<int> daemon_gs{0};  // daemon imps (excluded from completion check)

    // Quiescence hook: called by main_loop when all workers are parked
    // but imps are still alive.  Returns true to keep going (e.g.,
    // fake_clock advanced time), false to stop (real deadlock).
    std::function<bool()> quiescence_hook_;
    std::mutex hook_mu_;
    // Mirror of quiescence_hook_ != nullptr. Updated under hook_mu_ but
    // readable without holding hook_mu_ (e.g., inside park_cv.wait predicate
    // where acquiring hook_mu_ would violate the locking order with park_mu).
    std::atomic<bool> has_hook_{false};

    // Dynamic processor pool management.
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
    void quiescent_loop();
    void watchdog_loop();
    void add_processor();
    // Wake one parked/sleeping worker so it can steal from a stalled P.
    // Returns false when no worker is parked (caller may add_processor).
    // Watchdog-only: unlike unpark_one() it reports success and skips the
    // park_cv notifies (the watchdog isn't publishing new work).
    bool try_wake_parked_worker();
    Imp* local_next(Processor& p);
    bool take_from_global(Processor& p);
    bool steal_work(Processor& thief);
    bool has_work(Processor& p);
};

}
