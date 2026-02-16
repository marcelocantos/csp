#include <csp/internal/runtime.h>
#include <csp/stack_analysis.h>

#include <pthread.h>

#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <thread>

#include <stdlib.h>

extern "C" {

    // Return the current status message for the current microthread.
    const char* csp_getdescr(void* thr);

}


static std::function<void()> g_scheduler = []{
    while (csp_run()) { }
};


namespace csp {

    namespace detail {

        struct alignas(16) align_16 {
            char c[16];
        };

        static void vstatus(Microthread * mt, char const * msg, va_list args) {
            char * buf = mt->status_;
            int len = sizeof(mt->status_);
            int n = snprintf(buf, len, "§%lu ", mt->id_);
            vsnprintf(buf += n, len -= n, msg, args);
        }

        // After a context save completes, clear the suspended
        // microthread's suspending_ flag and drain any deferred
        // wake_pending_.  In M:N mode, both operations are done
        // under global_mu so they are mutually exclusive with
        // schedule()'s suspending_ check — eliminating the TOCTOU
        // race where schedule() sees suspending_==true and sets
        // wake_pending_, but the drain clears suspending_ and checks
        // wake_pending_ in between (seeing false both times).
        static void drain_suspended(Microthread* suspended) {
            auto& rt = Runtime::instance();
            if (rt.procs.size() > 1) {
                bool need_unpark = false;
                {
                    std::lock_guard<std::mutex> lk(rt.global_mu);
                    suspended->suspending_.store(false, std::memory_order_release);
                    if (suspended->wake_pending_.exchange(false, std::memory_order_acq_rel)) {
                        if (!suspended->in_global_) {
                            rt.push_to_global(suspended);
                            need_unpark = true;
                        }
                    }
                }
                if (need_unpark) {
                    rt.unpark_one();
                }
            } else {
                suspended->suspending_.store(false, std::memory_order_release);
            }
        }

        static intptr_t switch_to(Microthread & mt, intptr_t data) {
            auto self = g_self;
            // Acquire-load ctx_ to synchronize with the release-store
            // that saved the target's context on a (possibly different)
            // OS thread.  This ensures the saved register data on the
            // target's stack is visible to us before we jump.
            auto ctx = mt.ctx_.load(std::memory_order_acquire);
            current_p().save_ctx = &self->ctx_;
            current_p().save_mt = self;
#if CSP_TSAN
            __tsan_switch_to_fiber(mt.tsan_fiber_, 0);
#endif
            auto t = jump_fcontext(ctx, (void *)data);
            // Release-store our caller's saved SP so that any thread
            // that later acquire-loads ctx_ will also see the register
            // data that jump_fcontext wrote to the caller's stack.
            current_p().save_ctx->store(t.fctx, std::memory_order_release);
            drain_suspended(current_p().save_mt);
            return (intptr_t)t.data;
        }

        Microthread::Microthread(fcontext_t ctx, StackSlot * stk) : ctx_(ctx), stk_(stk) {
            prev_ = next_ = nullptr;
            snprintf(status_, sizeof(status_), "§%lu", id_);
        }

        Microthread::Microthread() : Microthread(nullptr, nullptr) {
            prev_ = next_ = this;
            snprintf(status_, sizeof(status_), "§main");
        }

        void Microthread::schedule_local(bool make_current) {
            std::lock_guard<std::mutex> lk(current_p().run_mu);
            if (next_) {
                return;
            }
            auto& busy = current_p().busy;
            if (busy) {
                next_ = busy;
                prev_ = busy->prev_;
                next_->prev_ = prev_->next_ = this;
                if (make_current) {
                    busy = this;
                }
            } else {
                busy = next_ = prev_ = this;
            }
        }

        void Microthread::schedule(bool make_current) {
            auto& rt = Runtime::instance();

            // In M:N mode, push to the global run queue so any worker
            // can pick it up, preventing stranding on a P whose worker
            // is about to park.
            if (rt.procs.size() > 1) {
                {
                    std::lock_guard<std::mutex> lk(rt.global_mu);
                    if (in_global_) {
                        return;
                    }
                    // If the microthread is in the unlock_all→do_switch
                    // window, it's still running and can't be safely
                    // pushed to the global queue.  Set wake_pending_
                    // so the detach path will re-add it to a queue.
                    if (suspending_.load(std::memory_order_acquire)) {
                        wake_pending_.store(true, std::memory_order_release);
                        return;
                    }
                    rt.push_to_global(this);
                }
                rt.unpark_one();
                return;
            }

            schedule_local(make_current);
        }

        void Microthread::deschedule() {
            std::lock_guard<std::mutex> lk(current_p().run_mu);
            assert(next_);
            auto& busy = current_p().busy;
            if (busy == this && (busy = next_) == this) {
                busy = nullptr;
            }
            if (next_) next_->prev_ = prev_;
            if (prev_) prev_->next_ = next_;
            next_ = nullptr;
            prev_ = nullptr;
        }

        void Microthread::run(Status status) {
            auto& p = current_p();
            auto& busy = p.busy;
            assert(this != g_self);
            auto self = g_self;

            // Manipulate run queue under lock, but release before context switch.
            {
                std::lock_guard<std::mutex> lk(p.run_mu);

                switch (status) {
                case Status::run:
                    break;
                case Status::sleep:
                    if (g_self == busy) {
                        busy = busy->next_;
                    }
                    break;
                case Status::detach:
                case Status::exit:
                    // Inline deschedule without re-acquiring run_mu.
                    assert(g_self->next_);
                    if (busy == g_self && (busy = g_self->next_) == g_self) {
                        busy = nullptr;
                    }
                    if (g_self->next_) g_self->next_->prev_ = g_self->prev_;
                    if (g_self->prev_) g_self->prev_->next_ = g_self->next_;
                    g_self->next_ = nullptr;
                    g_self->prev_ = nullptr;

                    if (status == Status::detach &&
                        g_self->wake_pending_.exchange(false, std::memory_order_acq_rel)) {
                        if (busy) {
                            g_self->next_ = busy;
                            g_self->prev_ = busy->prev_;
                            g_self->next_->prev_ = g_self->prev_->next_ = g_self;
                        } else {
                            busy = g_self->next_ = g_self->prev_ = g_self;
                        }
                        return;
                    }
                    break;
                default: ;
                }

                // Inline schedule without re-acquiring run_mu.
                if (!next_) {
                    if (busy) {
                        next_ = busy;
                        prev_ = busy->prev_;
                        next_->prev_ = prev_->next_ = this;
                    } else {
                        busy = next_ = prev_ = this;
                    }
                }
            }

            auto killme = status == Status::exit ? g_self : nullptr;
            auto killyou = reinterpret_cast<Microthread *>(switch_to(*this, reinterpret_cast<intptr_t>(killme)));
            if (killyou) {
#if CSP_TSAN
                if (killyou->tsan_fiber_) __tsan_destroy_fiber(killyou->tsan_fiber_);
#endif
                auto stk = killyou->stk_;
                killyou->~Microthread();
                delete [] stk;
                auto& rt = Runtime::instance();
                if (rt.live_gs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    // Lock park_mu briefly to synchronize with main_loop's
                    // wait, preventing missed notifications.
                    { std::lock_guard<std::mutex> lk(rt.park_mu); }
                    rt.park_cv.notify_all();
                }
            }

            if (!killme) {
                g_self = self;
            }
        }

        void do_switch(Status status) {
            Microthread* target;
            {
                std::lock_guard<std::mutex> lk(current_p().run_mu);
                // Update running to the active MT so steal_work skips it.
                // (local_next sets running for the initial pick; chained
                // do_switch calls keep it current as execution moves
                // between microthreads.)
                current_p().running = g_self;
                auto& busy = current_p().busy;
                if (busy == g_self) {
                    busy = busy->next_;
                }
                target = busy;
            }
            target->run(status);
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
    if (current_p().save_ctx) {
        current_p().save_ctx->store(t.fctx, std::memory_order_release);
        drain_suspended(current_p().save_mt);
    }
    // Copy all data from StartData before the warmup switch, because
    // StartData lives on the spawner's stack and may be freed before
    // this microthread resumes (in M:N mode, resumption happens on a
    // different OS thread after the spawner has returned).
    auto & sd = *reinterpret_cast<StartData const *>(t.data);
    auto start_f = sd.start_f;
    auto data = sd.data;
    auto * self = &sd.self;
    g_self = self;
    auto killyou_val = switch_to(sd.caller, 0);
    // After warmup switch, sd may be invalid. Use local copies only.
    g_self = self;

    // In M:N mode, the resuming switch may carry a killyou pointer — a
    // dying microthread that exited and chained into us via run(exit).
    // Clean it up before running our own function.
    if (auto* killyou = reinterpret_cast<Microthread*>(killyou_val)) {
#if CSP_TSAN
        if (killyou->tsan_fiber_) __tsan_destroy_fiber(killyou->tsan_fiber_);
#endif
        auto stk = killyou->stk_;
        killyou->~Microthread();
        delete [] stk;
        auto& rt = Runtime::instance();
        if (rt.live_gs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            { std::lock_guard<std::mutex> lk(rt.park_mu); }
            rt.park_cv.notify_all();
        }
    }

    try {
        start_f(data);
    } catch (std::exception const & e) {
    } catch (...) {
    }
    do_switch(Status::exit);
};

int csp_spawn(void (*start_f)(void *), void * data) {
    (void)current_p(); // Ensure g_self is bound before use.
    try {
        // Analyze stack depth to right-size the allocation.
        // Disabled under sanitizers: instrumented code has shadow-memory
        // checking branches that cause the walker to explode.
        size_t S = Microthread::stack_size / 16; // default: full 32KB
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) && \
    !(defined(__has_feature) && (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer)))
        {
            csp::stack_analysis_options spawn_opts;
            spawn_opts.max_instructions = 10000;
            spawn_opts.max_call_depth = 16;
            auto fn_ptr = reinterpret_cast<const void*>(start_f);
            csp::stack_analysis analysis;
            if (!g_self->stk_) {
                // System thread (8MB stack): run full analysis directly.
                analysis = csp::analyze_stack_depth(fn_ptr, data, spawn_opts);
            } else {
                // Microthread: cache-only lookup (the analyzer itself
                // needs deep stack space, so we can't run it here).
                analysis = csp::analyze_stack_depth_cached(
                    fn_ptr, data, spawn_opts);
            }
            if (analysis.is_exact) {
                size_t stack_bytes = analysis.max_depth + 4096;
                stack_bytes = std::max(stack_bytes, size_t{8192});
                stack_bytes = std::min(stack_bytes, Microthread::stack_size);
                S = (stack_bytes + 15) / 16;
            }
        }
#endif
        auto stk = new Microthread::StackSlot[S];
        auto mt = (Microthread *)(stk + S) - 1;
        assert(((uintptr_t)mt % 16) == 0); // Must be 16-byte aligned.
        auto ctx = make_fcontext(mt, (char *)mt - (char *)stk, start);
        new (mt) Microthread(ctx, stk);
#if CSP_TSAN
        mt->tsan_fiber_ = __tsan_create_fiber(0);
#endif

        StartData const start_data = {start_f, data, *mt, *g_self};
        auto self = g_self;
        switch_to(*mt, reinterpret_cast<intptr_t>(&start_data));
        g_self = self;

        auto& rt = Runtime::instance();
        rt.live_gs.fetch_add(1, std::memory_order_relaxed);

        if (rt.procs.size() > 1) {
            // M:N mode: after the handshake switch_to, mt is initialized
            // and suspended but NOT on any run queue. Push it to the
            // global queue for workers to pick up and run.
            {
                std::lock_guard<std::mutex> lk(rt.global_mu);
                rt.push_to_global(mt);
            }
            rt.park_cv.notify_all();
        } else {
            // Single-P mode: run the microthread on the main thread
            // (original behavior — run until it yields).
            mt->run(Status::run);
            rt.unpark_one();
        }

        return 1;
    } catch (std::exception const & e) {
        return 0;
    } catch (...) {
        return 0;
    }
}

void csp_sleep_until(int64_t deadline_ns) {
    using namespace std::chrono;
    auto deadline = steady_clock::time_point(nanoseconds(deadline_ns));
    current_p().timer_heap.push({deadline, g_self});
    g_self->suspending_.store(true, std::memory_order_release);
    do_switch(Status::detach);
    g_self->suspending_.store(false, std::memory_order_release);
}

int csp_run() {
    auto& p = current_p();
    auto& timer_heap = p.timer_heap;

    // Fire expired timers — reschedule their microthreads.
    {
        auto now = std::chrono::steady_clock::now();
        while (!timer_heap.empty() && timer_heap.top().deadline <= now) {
            auto mt = timer_heap.top().thread;
            timer_heap.pop();
            mt->schedule_local();
        }
    }

    Microthread* target = nullptr;
    bool has_timers = false;
    {
        std::lock_guard<std::mutex> lk(p.run_mu);
        auto& busy = p.busy;
        if (busy == g_self) {
            busy = busy->next_;
        }
        if (busy != g_self) {
            target = busy;
        }
        has_timers = !timer_heap.empty();
    }

    if (target) {
        target->run();
    } else if (has_timers) {
        // All microthreads blocked, but timers pending — sleep until next deadline.
        std::this_thread::sleep_until(timer_heap.top().deadline);
    }

    {
        std::lock_guard<std::mutex> lk(p.run_mu);
        return p.busy->next_ != p.busy || !timer_heap.empty();
    }
}

void csp_yield() {
    bool should_switch;
    {
        std::lock_guard<std::mutex> lk(current_p().run_mu);
        auto& busy = current_p().busy;
        should_switch = busy->next_ != busy;
    }
    if (should_switch) {
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
