#pragma once

#include <csp/internal/csp_internal.h>

#include <chrono>
#include <mutex>
#include <queue>
#include <vector>

namespace csp::detail {

struct TimerEntry {
    std::chrono::steady_clock::time_point deadline;
    Imp * thread;
    bool operator>(TimerEntry const & o) const { return deadline > o.deadline; }
};

struct Processor {
    Imp  main;       // Sentinel node for this P's run queue
    Imp* busy;       // Head of circular DLL run queue
    std::atomic<fcontext_t>*  save_ctx;   // Where to store suspended imp's ctx
    Imp*  save_imp;    // The imp being suspended

    std::priority_queue<TimerEntry, std::vector<TimerEntry>,
                        std::greater<TimerEntry>> timer_heap;

    std::mutex run_mu;                // Protects busy queue DLL + timer_heap
    Imp* running = nullptr;   // Imp claimed by local_next (steal-safe)
    std::atomic<bool> parked{false};  // Is this P's worker thread parked?

    std::atomic<uint64_t> heartbeat{0};  // Incremented each worker_loop iter
    std::atomic<bool> alive{true};       // False when surplus worker exits

    int id;

    Processor(int id_)
        : busy(&main)
        , save_ctx(nullptr)
        , save_imp(nullptr)
        , id(id_)
    { }

    Processor(Processor const &) = delete;
    Processor& operator=(Processor const &) = delete;
};

Processor& current_p();
void bind_processor(Processor* p);

}
