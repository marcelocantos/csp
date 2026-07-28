#pragma once

#include <csp/internal/csp_internal.h>
#include <csp/internal/fast_mutex.h>
#include <csp/internal/note.h>

#include <thread>

namespace csp::detail {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4324) // structure was padded due to alignment specifier
#endif
struct Processor {
    Imp  main;       // Sentinel node for this P's run queue
    Imp* busy;       // Head of circular DLL run queue
    std::atomic<fcontext_t>*  save_ctx;   // Where to store suspended imp's ctx
    Imp*  save_imp;    // The imp being suspended

    FastMutex run_mu;                 // Protects busy queue DLL
    Imp* running = nullptr;   // Imp claimed by local_next (steal-safe)
    // Fairness budget for wake-to-local (🎯T34 O1): consecutive local
    // wake count; every kLocalWakeBudget-th wake routes to the global
    // queue instead, so a hot rendezvous pair cannot monopolize this P
    // while spawned/global work goes underserved (paper 33; the
    // flat_map balloon). Mutated under run_mu.
    int local_wake_streak_ = 0;
    std::atomic<bool> parked{false};  // Is this P's worker thread parked?
    Note note;                        // Per-worker futex note (park/unpark)

    // Own cache line: written every worker_loop iteration; sharing a
    // line with thief-scanned fields (parked, note) would invalidate
    // their cached copies on every heartbeat (🎯T35 cache pass).
    alignas(64) std::atomic<uint64_t> heartbeat{0};  // Incremented each worker_loop iter
    std::atomic<bool> alive{true};       // False when surplus worker exits

    std::thread worker;                   // Worker thread (empty for P0/main)

    int id;

    Processor(int id_)
        : busy(&main)
        , save_ctx(nullptr)
        , save_imp(nullptr)
        , id(id_)
    { }

    // True when the ring holds waiting work besides the sentinel and the
    // currently running imp. Caller must hold run_mu (🎯T49: shared by
    // the wake-to-local fairness check in place_on_run_queue and the
    // watchdog's rescuable scan).
    bool ring_has_waiting() const {
        if (auto* start = busy) {
            auto* it = start;
            do {
                if (it != &main && it != running) {
                    return true;
                }
                it = it->next_;
            } while (it != start);
        }
        return false;
    }

    // Reinitialize a dead surplus P for reuse.  Caller must have
    // joined the old worker thread first.
    void reset() {
        busy = &main;
        save_ctx = nullptr;
        save_imp = nullptr;
        running = nullptr;
        parked.store(false, std::memory_order_relaxed);
        heartbeat.store(0, std::memory_order_relaxed);
        note.reset();
        // alive must be last — steal_work reads it with acquire.
        alive.store(true, std::memory_order_release);
    }

    Processor(Processor const &) = delete;
    Processor& operator=(Processor const &) = delete;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

Processor& current_p();
void bind_processor(Processor* p);

// Returns true if the calling thread has a bound Processor.
// False on the reactor thread and other external threads.
bool has_processor();

}
