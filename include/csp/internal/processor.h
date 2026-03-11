#pragma once

#include <csp/internal/csp_internal.h>

#include <mutex>
#include <thread>

namespace csp::detail {

struct Processor {
    Imp  main;       // Sentinel node for this P's run queue
    Imp* busy;       // Head of circular DLL run queue
    std::atomic<fcontext_t>*  save_ctx;   // Where to store suspended imp's ctx
    Imp*  save_imp;    // The imp being suspended

    std::mutex run_mu;                // Protects busy queue DLL
    Imp* running = nullptr;   // Imp claimed by local_next (steal-safe)
    std::atomic<bool> parked{false};  // Is this P's worker thread parked?

    std::atomic<uint64_t> heartbeat{0};  // Incremented each worker_loop iter
    std::atomic<bool> alive{true};       // False when surplus worker exits

    std::thread worker;                   // Worker thread (empty for P0/main)

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

// Returns true if the calling thread has a bound Processor.
// False on the reactor thread and other external threads.
bool has_processor();

}
