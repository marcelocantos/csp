#ifndef INCLUDED__csp__internal__processor_h
#define INCLUDED__csp__internal__processor_h

#include <csp/internal/microthread_internal.h>

#include <chrono>
#include <mutex>
#include <queue>
#include <vector>

namespace csp {

    namespace detail {

        struct TimerEntry {
            std::chrono::steady_clock::time_point deadline;
            Microthread * thread;
            bool operator>(TimerEntry const & o) const { return deadline > o.deadline; }
        };

        struct Processor {
            Microthread  main;       // Sentinel node for this P's run queue
            Microthread* busy;       // Head of circular DLL run queue
            std::atomic<fcontext_t>*  save_ctx;   // Where to store suspended mt's ctx
            Microthread*  save_mt;    // The microthread being suspended

            std::priority_queue<TimerEntry, std::vector<TimerEntry>,
                                std::greater<TimerEntry>> timer_heap;

            std::mutex run_mu;                // Protects the busy queue DLL
            Microthread* running = nullptr;   // MT claimed by local_next (steal-safe)
            std::atomic<bool> parked{false};  // Is this P's worker thread parked?

            int id;

            Processor(int id_)
                : busy(&main)
                , save_ctx(nullptr)
                , save_mt(nullptr)
                , id(id_)
            { }

            Processor(Processor const &) = delete;
            Processor& operator=(Processor const &) = delete;
        };

        Processor& current_p();
        void bind_processor(Processor* p);

    }

}

#endif // INCLUDED__csp__internal__processor_h
