#include <csp/internal/runtime.h>
#include <csp/internal/hamt.h>

#include <pthread.h>

#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <exception>
#include <thread>

#include <stdlib.h>

static std::function<void()> g_scheduler = []{
    while (csp::internal::run()) { }
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
            if (rt.mn_mode_) {
                bool need_unpark = false;
                {
                    std::lock_guard<std::mutex> lk(rt.global_mu); // TLA:DrainSuspended.AcquireDrain
                    // TLA:DrainSuspended.Drain
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

        Microthread::Microthread(fcontext_t ctx, StackRegion stk) : ctx_(ctx), stk_(stk) {
            prev_ = next_ = nullptr;
            snprintf(status_, sizeof(status_), "§%lu", id_);
        }

        Microthread::Microthread() : Microthread(nullptr, {}) {
            prev_ = next_ = this;
            snprintf(status_, sizeof(status_), "§main");
        }

        Microthread::~Microthread() {
            if (dyn_ctx_) csp::internal::hamt_release(dyn_ctx_);
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
            // TLA:DrainSuspended.AcquireWake TLA:StealWork.WStartSchedule TLA:StealWork.WAcquireLock
            if (rt.mn_mode_) {
                {
                    std::lock_guard<std::mutex> lk(rt.global_mu);
                    if (in_global_) {
                        return;
                    }
                    // TLA:DrainSuspended.DoSchedule
                    // If the microthread is in the unlock_all→do_switch
                    // window, it's still running and can't be safely
                    // pushed to the global queue.  Set wake_pending_
                    // so the detach path will re-add it to a queue.
                    if (suspending_.load(std::memory_order_acquire)) {
                        wake_pending_.store(true, std::memory_order_release);
                        return;
                    }
                    rt.push_to_global(this); // TLA:StealWork.WPush
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

        static void destroy_microthread(Microthread* mt) {
#if CSP_TSAN
            if (mt->tsan_fiber_) __tsan_destroy_fiber(mt->tsan_fiber_);
#endif
            auto region = mt->stk_;
            mt->~Microthread();
            StackPool::instance().release(region);
            auto& rt = Runtime::instance();
            if (rt.live_gs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                { std::lock_guard<std::mutex> lk(rt.park_mu); }
                rt.park_cv.notify_all();
            }
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
                case Status::detach: // TLA:StealWork.VDeschedule
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

                    // TLA:DrainSuspended.CheckWP
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
                destroy_microthread(killyou);
            }

            if (!killme) {
                g_self = self;
            }
        }

        // TLA:StealWork.VDoSwitch
        void do_switch(Status status) {
            // Reclaim unused stack pages before suspending.
            if (g_self->stk_) {
                StackPool::instance().maybe_shrink(
                    g_self->stk_, __builtin_frame_address(0));
            }
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
    auto parent_dyn_ctx = sd.caller.dyn_ctx_;
    g_self = self;
    auto killyou_val = switch_to(sd.caller, 0);
    // After warmup switch, sd may be invalid. Use local copies only.
    g_self = self;
    self->dyn_ctx_ = parent_dyn_ctx;
    if (parent_dyn_ctx) csp::internal::hamt_retain(parent_dyn_ctx);

    // In M:N mode, the resuming switch may carry a killyou pointer — a
    // dying microthread that exited and chained into us via run(exit).
    // Clean it up before running our own function.
    if (auto* killyou = reinterpret_cast<Microthread*>(killyou_val)) {
        destroy_microthread(killyou);
    }

    try {
        start_f(data);
    } catch (std::exception const & e) {
    } catch (...) {
    }
    do_switch(Status::exit);
};

namespace csp::internal {

int spawn(EntryFn start_f, void * data) {
    (void)current_p(); // Ensure g_self is bound before use.
    // Reclaim unused stack pages at this API boundary.
    if (g_self->stk_) {
        StackPool::instance().maybe_shrink(
            g_self->stk_, __builtin_frame_address(0));
    }
    try {
#if CSP_USE_MMAP_STACKS
        auto& pool = StackPool::instance();
        auto region = pool.allocate();
        auto page_sz = pool.page_size();
        auto* top = static_cast<char*>(region.base) + region.total_size;
        auto* mt = reinterpret_cast<Microthread*>(top) - 1;
        assert(((uintptr_t)mt % 16) == 0);
        auto* usable_base = static_cast<char*>(region.base) + page_sz;
        auto ctx = make_fcontext(mt, (char*)mt - usable_base, start);
        new (mt) Microthread(ctx, region);
#else
        // Under sanitizers: heap-allocate with stack analyzer right-sizing.
        auto region = StackPool::instance().allocate();
        size_t S = region.total_size / 16;
        auto* stk = static_cast<Microthread::StackSlot*>(region.base);
        auto* mt = reinterpret_cast<Microthread*>(stk + S) - 1;
        assert(((uintptr_t)mt % 16) == 0);
        auto ctx = make_fcontext(mt, (char*)mt - (char*)stk, start);
        new (mt) Microthread(ctx, region);
#endif
#if CSP_TSAN
        mt->tsan_fiber_ = __tsan_create_fiber(0);
#endif

        StartData const start_data = {start_f, data, *mt, *g_self};
        auto self = g_self;
        switch_to(*mt, reinterpret_cast<intptr_t>(&start_data));
        g_self = self;

        auto& rt = Runtime::instance();
        rt.live_gs.fetch_add(1, std::memory_order_relaxed);

        if (rt.mn_mode_) {
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

void sleep_until(int64_t deadline_ns) {
    using namespace std::chrono;
    auto deadline = steady_clock::time_point(nanoseconds(deadline_ns));
    {
        std::lock_guard<std::mutex> lk(current_p().run_mu);
        current_p().timer_heap.push({deadline, g_self});
    }
    g_self->suspending_.store(true, std::memory_order_release); // TLA:DrainSuspended.BeginSuspend
    do_switch(Status::detach);
    g_self->suspending_.store(false, std::memory_order_release); // TLA:DrainSuspended.ClearSusp
}

int run() {
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

void yield() {
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

void descr(char const * fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vstatus(g_self, fmt, args);
    va_end(args);

    pthread_setname_np(getstatus(g_self));
}

char const * get_descr(void * thr) {
    return getfullstatus(thr ? static_cast<Microthread const *>(thr) : g_self);
}

} // namespace csp::internal
