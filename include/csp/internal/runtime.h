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
    // Watcher-count gates for park_cv broadcasts (🎯T34 O3).  Only
    // completion/quiescence watchers (main_loop, run, await_idle,
    // await_quiescent, quiescent_loop) wait on park_cv; scheduler-side
    // notifiers skip the broadcast syscall when none is registered.
    // Two classes, two instances of the gate protocol verified in
    // formal/ParkGate.tla:
    //   park_waiters_    — all watchers; notified on completion events
    //                      (imp exit, shutdown).
    //   quiesce_waiters_ — the subset whose predicate reads worker
    //                      parked state; notified when a worker parks
    //                      or winds down.
    // A watcher's predicate must only read state whose publishers
    // notify a counter the watcher registered on.
    std::atomic<int> park_waiters_{0};
    std::atomic<int> quiesce_waiters_{0};

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

    // Register as a park_cv watcher and wait for pred.  quiescence
    // selects the additional quiesce_waiters_ registration for
    // predicates that read worker parked state.
    // TLA:ParkGate.WRegister TLA:ParkGate.WEval
    template <typename Pred>
    void park_wait(Pred&& pred, bool quiescence = false) {
        std::unique_lock<std::mutex> lk(park_mu);
        park_waiters_.fetch_add(1, std::memory_order_relaxed);
        if (quiescence) {
            quiesce_waiters_.fetch_add(1, std::memory_order_relaxed);
        }
        // Pairs with the fence in notify_watchers (eventcount): either
        // the notifier sees our registration, or our predicate read
        // sees its published state.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        park_cv.wait(lk, std::forward<Pred>(pred));
        if (quiescence) {
            quiesce_waiters_.fetch_sub(1, std::memory_order_relaxed);
        }
        park_waiters_.fetch_sub(1, std::memory_order_relaxed);
    }

    // Broadcast park_cv iff a watcher is registered.  Callers must
    // have published the state the watchers' predicates read (release
    // stores suffice; the fence supplies the SC pairing).
    void notify_watchers();          // completion events (imp exit, shutdown)
    void notify_quiesce_watchers();  // worker park / wind-down events

    // Unconditional broadcast for cold reconfiguration events
    // (quiescence-hook install/uninstall): wakes every watcher so it
    // re-enters park_wait and re-registers with fresh parameters.
    void broadcast_park();

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
