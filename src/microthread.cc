#include <csp/internal/microthread_internal.h>

#include <pthread.h>

#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <exception>
#include <iostream>
#include <map>
#include <queue>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <stdlib.h>

extern "C" {

    // Return the current status message for the current microthread.
    const char* csp_getdescr(void* thr);

}

static Logger g_log     ("microthread/-");
static Logger g_tmp     ("tmp/-");
static Logger g_inout   ("microthread/inout");
static Logger g_busyq   ("microthread/busyq");
static Logger g_current ("microthread/current");
static Logger g_stacklog("microthread/stack");
static Logger g_spawnlog("microthread/spawn-stack");
static Logger g_sequence("microthread/sequence");

// Use OS threads instead of green threads.
static Logger g_os_threads("__os_threads__");

static std::function<void()> g_scheduler = []{
    while (csp_run()) { }
};

namespace {

    struct TimerEntry {
        std::chrono::steady_clock::time_point deadline;
        csp::detail::Microthread * thread;
        bool operator>(TimerEntry const & o) const { return deadline > o.deadline; }
    };

    // Min-heap: earliest deadline on top.
    std::priority_queue<TimerEntry, std::vector<TimerEntry>, std::greater<TimerEntry>> g_timer_heap;

}

namespace csp {

    namespace detail {

        struct alignas(16) align_16 {
            char c[16];
        };

        static std::string qdescr(Microthread const * mt) {
            std::ostringstream oss;
            if (mt) {
                oss << getstatus(mt);
                for (auto c = mt; (c = c->next_) != mt;) {
                    oss << " → " << getstatus(c);
                }
            } else {
                oss << "∎";
            }
            return oss.str();
        }

        static void vstatus(Microthread * mt, char const * msg, va_list args) {
            char * buf = mt->status_;
            int len = sizeof(mt->status_);
            int n = snprintf(buf, len, "§%lu ", mt->id_);
            vsnprintf(buf += n, len -= n, msg, args);
        }

        // Before jumping, the source sets this to &own_ctx_ so the
        // target can save the source's context upon landing.
        static fcontext_t * g_save_ctx_here = nullptr;

        static intptr_t switch_to(Microthread & mt, intptr_t data) {
            auto self = g_self;                                         if (g_stacklog) { CSP_LOG(g_stacklog, "switching"); Logger::dump_stack(); }
            ;                                                           if (g_sequence) { std::cerr << "deactivate " << g_self->id_ << "\n"; std::cerr << "activate " << mt.id_ << "\n"; }
            intptr_t result;
            if (g_os_threads) {
                mt.notify(data);
                result = self->wait();
            } else {
                g_save_ctx_here = &self->ctx_;
                auto t = jump_fcontext(mt.ctx_, (void *)data);
                *g_save_ctx_here = t.fctx;
                result = (intptr_t)t.data;
            }                                                           CSP_LOG(g_current, "---- SWITCHED ----", getstatus(g_self));
            return result;
        }

        Microthread::Microthread(fcontext_t ctx, StackSlot * stk) : ctx_(ctx), stk_(stk) {
            prev_ = next_ = nullptr;
            snprintf(status_, sizeof(status_), "§%lu", id_);
        }

        Microthread::Microthread() : Microthread(nullptr, nullptr) {
            prev_ = next_ = this;
            snprintf(status_, sizeof(status_), "§main");
        }

        void Microthread::schedule(bool make_current) {                 CSP_LOG(g_busyq, "schedule %s [%s]", getstatus(this), qdescr(g_busy).c_str());
            if (next_) {
                return;
            }
            if (g_busy) {
                next_ = g_busy;
                prev_ = g_busy->prev_;
                next_->prev_ = prev_->next_ = this;
                if (make_current) {
                    g_busy = this;
                }
            } else {
                g_busy = next_ = prev_ = this;
            }                                                           CSP_LOG(g_busyq, "  g_busy = [%s]", qdescr(g_busy).c_str());
        }

        void Microthread::deschedule() {                                CSP_LOG(g_busyq, "deschedule %s", getstatus(this));
            assert(next_);
            if (g_busy == this && (g_busy = next_) == this) {
                g_busy = nullptr;
            }
            if (next_) next_->prev_ = prev_;
            if (prev_) prev_->next_ = next_;
            next_ = nullptr;
            prev_ = nullptr;
        }

        void Microthread::run(Status status) {                          CSP_LOG(g_inout, "/=== ENTER %s->Microthread::run(%s, %lu) ===", getstatus(g_self), getstatus(this), status);
            ;                                                           CSP_LOG(g_busyq, "run [%s]", qdescr(g_busy).c_str());
            assert(this != g_self);
            auto self = g_self;

            switch (status) {
            case Status::run:
                break;
            case Status::sleep:
                if (g_self == g_busy) {
                    g_busy = g_busy->next_;                             CSP_LOG(g_busyq, "sleeping: [%s]", qdescr(g_busy).c_str());
                }
                break;
            case Status::detach:
            case Status::exit:
                g_self->deschedule();
                break;
            default: ;
            }

            schedule();

            auto killme = status == Status::exit ? g_self : nullptr;
                                                                        CSP_LOG(g_inout, "Switch to %s", getstatus(this));
            auto killyou = reinterpret_cast<Microthread *>(switch_to(*this, reinterpret_cast<intptr_t>(killme)));
                                                                        CSP_LOG(g_log, "jump_fcontext() → %s (%s)", killme ? getstatus(killme) : "-", getstatus(g_busy));
            if (killyou) {                                              CSP_LOG(g_log, "kill %s (stk = %p)", getstatus(killyou), killyou->stk_);
                if (g_os_threads) {
                    killyou->notify(0);
                    killyou->os_thread_.join();
                }
                auto stk = killyou->stk_;
                killyou->~Microthread();
                delete [] stk;
            }                                                           CSP_LOG(g_busyq, "Busy queue: [%s]", qdescr(g_busy).c_str());
                                                                        CSP_LOG(g_inout, "=== EXIT Microthread::run ===/");

            if (!killme) {
                g_self = self;
            }
        }

        void do_switch(Status status) {
            if (g_busy == g_self) {
                g_busy = g_busy->next_;
            }
            g_busy->run(status);
        }

    }

    void set_scheduler(std::function<void()> scheduler) {
        g_scheduler = std::move(scheduler);
    }

    void schedule() {
        g_scheduler();
    }

}

using namespace csp::detail;


namespace {

    struct StartData {
        void (* start_f)(void *);
        void * data;
        Microthread & self;
        Microthread & caller;
    };

}

static void start(transfer_t t) {
    if (g_save_ctx_here) *g_save_ctx_here = t.fctx;
    auto & start = *reinterpret_cast<StartData const *>(t.data);
    g_self = &start.self;                                               CSP_LOG(g_inout, "/=== ENTER ===");
    switch_to(start.caller, 0);                                         CSP_LOG(g_inout, " --- RUNNING ---");
    g_self = &start.self;
    try {
        start.start_f(start.data);
    } catch (std::exception const & e) {                                CSP_GRIPE(g_log, "Uncaught exception: %s", e.what());
    } catch (...) {                                                     CSP_GRIPE(g_log, "Uncaught exception of unknown type");
    }                                                                   CSP_LOG(g_inout, " === EXIT ===/");
    do_switch(Status::exit);
};

int csp_spawn(void (*start_f)(void *), void * data) {
    try {
        ;                                                               if (g_sequence) { static std::once_flag once; std::call_once(once, [] { std::cerr << "activate " << g_self->id_ << "\n"; }); }
        constexpr size_t S = Microthread::stack_size / 16;
        auto stk = new Microthread::StackSlot[S];
        auto mt = (Microthread *)(stk + S) - 1;
        assert(((uintptr_t)mt % 16) == 0); // Must be 16-byte aligned.
        auto ctx = make_fcontext(mt, (char *)mt - (char *)stk, start);
        new (mt) Microthread(ctx, stk);

        StartData const start_data = {start_f, data, *mt, *g_self};     CSP_LOG(g_log, "starting %s (stk = %p)", getstatus(mt), stk);
                                                                        if (g_spawnlog) { CSP_LOG(g_spawnlog, "spawning %s", getstatus(mt)); Logger::dump_stack(); }
        if (g_os_threads) {
            mt->os_thread_ = std::thread([mt]{                          CSP_LOG(g_log, "started OS thread");
                pthread_setname_np(getstatus(mt));
                start(transfer_t{nullptr, (void *)mt->wait()});
            });
        }
        auto self = g_self;
        switch_to(*mt, reinterpret_cast<intptr_t>(&start_data));
        g_self = self;                                                  CSP_LOG(g_log, "started %s", getstatus(mt));

        mt->run(Status::run);                                           CSP_LOG(g_log, "warmed up %s", getstatus(mt));

        return 1;
    } catch (...) { // TODO: Report the error somehow.
        return 0;
    }
}

void csp_sleep_until(int64_t deadline_ns) {
    using namespace std::chrono;
    auto deadline = steady_clock::time_point(nanoseconds(deadline_ns));
    g_timer_heap.push({deadline, g_self});
    do_switch(Status::detach);
}

int csp_run() {
    // Fire expired timers — reschedule their microthreads.
    {
        auto now = std::chrono::steady_clock::now();
        while (!g_timer_heap.empty() && g_timer_heap.top().deadline <= now) {
            auto mt = g_timer_heap.top().thread;
            g_timer_heap.pop();
            mt->schedule();
        }
    }

    if (g_busy == g_self) {
        g_busy = g_busy->next_;                                         CSP_LOG(g_busyq, "skipped %s: [%s]", getstatus(g_self), qdescr(g_busy).c_str());
    }
    if (g_busy != g_self) {
        g_busy->run();
    } else if (!g_timer_heap.empty()) {
        // All microthreads blocked, but timers pending — sleep until next deadline.
        std::this_thread::sleep_until(g_timer_heap.top().deadline);
    }
    return g_busy->next_ != g_busy || !g_timer_heap.empty();
}

void csp_yield() {
    if (g_busy->next_ != g_busy) {
        do_switch();
    }
}

void csp_descr(char const * fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vstatus(g_self, fmt, args);
    va_end(args);

    pthread_setname_np(getstatus(g_self));
}

char const * csp_getdescr(void * thr) {
    return getfullstatus(thr ? static_cast<Microthread const *>(thr) : g_self);
}
