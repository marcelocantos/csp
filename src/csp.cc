#include <csp/internal/runtime.h>
#include <csp/internal/reactor.h>
#include <csp/internal/hamt.h>

#ifndef _WIN32
#include <pthread.h>
#endif

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <exception>
#include <thread>
#include <utility>

#include <stdlib.h>

static void default_scheduler_impl() {
    auto& rt = csp::detail::Runtime::instance();
    while (true) {
        if (csp::internal::run()) continue;
        if (rt.live_gs.load(std::memory_order_acquire) == 0) break;
        // In single-P mode, if no reactor signals are pending and no
        // global work is queued, no external event can wake blocked
        // imps — exit (deadlock or done).  Both conditions must hold:
        // the reactor decrements pending_signals *after* scheduling
        // the woken imp (which sets has_global_work_), so checking
        // both avoids a race where pending_signals is already zero
        // but the woken imp hasn't been run yet.
        if (!rt.mn_mode_
            && !csp::detail::Reactor::instance().has_pending_signals()
            && !rt.has_global_work_.load(std::memory_order_acquire)) break;
        // Park until work arrives or all imps have exited.
        std::unique_lock<std::mutex> lk(rt.park_mu);
        rt.park_cv.wait(lk, [&rt] {
            return rt.live_gs.load(std::memory_order_acquire) == 0
                || rt.has_global_work_.load(std::memory_order_acquire);
        });
    }
    // Shut down the reactor so its thread doesn't outlive the scheduler.
    csp::detail::Reactor::instance().shutdown();
}

static std::function<void()> g_scheduler = default_scheduler_impl;


namespace csp {

    namespace detail {

        struct alignas(16) align_16 {
            char c[16];
        };

        static void vstatus(Imp * imp, char const * msg, va_list args) {
            char * buf = imp->status_;
            int len = sizeof(imp->status_);
            int n = snprintf(buf, len, "§%zu ", imp->id_);
            vsnprintf(buf += n, len -= n, msg, args);
        }

        // After a context save completes, clear the suspended
        // imp's suspending_ flag and drain any deferred
        // wake_pending_.  In M:N mode, both operations are done
        // under global_mu so they are mutually exclusive with
        // schedule()'s suspending_ check — eliminating the TOCTOU
        // race where schedule() sees suspending_==true and sets
        // wake_pending_, but the drain clears suspending_ and checks
        // wake_pending_ in between (seeing false both times).
        static void drain_suspended(Imp* suspended) {
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

        static intptr_t switch_to(Imp & target, intptr_t data) {
            auto self = current_imp();
            // Acquire-load ctx_ to synchronize with the release-store
            // that saved the target's context on a (possibly different)
            // OS thread.  This ensures the saved register data on the
            // target's stack is visible to us before we jump.
            auto ctx = target.ctx_.load(std::memory_order_acquire);
            current_p().save_ctx = &self->ctx_;
            current_p().save_imp = self;
#if CSP_ASAN
            __sanitizer_start_switch_fiber(
                &self->asan_fake_stack_,
                target.stk_.base, target.stk_.total_size);
#endif
#if CSP_TSAN
            __tsan_switch_to_fiber(target.tsan_fiber_, 0);
#endif
            auto t = jump_fcontext(ctx, (void *)data);
#if CSP_ASAN
            __sanitizer_finish_switch_fiber(
                self->asan_fake_stack_, nullptr, nullptr);
#endif
            // Release-store our caller's saved SP so that any thread
            // that later acquire-loads ctx_ will also see the register
            // data that jump_fcontext wrote to the caller's stack.
            current_p().save_ctx->store(t.fctx, std::memory_order_release);
            drain_suspended(current_p().save_imp);
            return (intptr_t)t.data;
        }

        Imp::Imp(fcontext_t ctx, StackRegion stk) : ctx_(ctx), stk_(stk) {
            prev_ = next_ = nullptr;
            snprintf(status_, sizeof(status_), "§%zu", id_);
        }

        Imp::Imp() : Imp(nullptr, {}) {
            prev_ = next_ = this;
            snprintf(status_, sizeof(status_), "§main");
        }

        Imp::~Imp() {
            if (dyn_ctx_) csp::internal::hamt_release(dyn_ctx_);
            delete local_ctx_;
        }

        void Imp::schedule_local(bool make_current) {
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

        void Imp::schedule(bool make_current) {
            auto& rt = Runtime::instance();

            // In M:N mode or when called from a thread without a
            // Processor (e.g. reactor thread), push to the global
            // run queue so any worker can pick it up.
            // TLA:DrainSuspended.AcquireWake TLA:StealWork.WStartSchedule TLA:StealWork.WAcquireLock
            if (rt.mn_mode_ || !has_processor()) {
                {
                    std::lock_guard<std::mutex> lk(rt.global_mu);
                    if (in_global_) {
                        return;
                    }
                    // TLA:DrainSuspended.DoSchedule
                    // If the imp is in the unlock_all→do_switch
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

        void Imp::deschedule() {
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

        static void destroy_imp(Imp* imp) {
#if CSP_TSAN
            if (imp->tsan_fiber_) __tsan_destroy_fiber(imp->tsan_fiber_);
#endif
            auto region = imp->stk_;
            imp->~Imp();
            StackPool::instance().release(region);
            auto& rt = Runtime::instance();
            if (rt.live_gs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                { std::lock_guard<std::mutex> lk(rt.park_mu); }
                rt.park_cv.notify_all();
            }
        }

        void Imp::run(Status status) {
            auto& p = current_p();
            auto& busy = p.busy;
            auto self = current_imp();
            assert(this != self);

            // Manipulate run queue under lock, but release before context switch.
            {
                std::lock_guard<std::mutex> lk(p.run_mu);

                switch (status) {
                case Status::run:
                    break;
                case Status::sleep:
                    if (self == busy) {
                        busy = busy->next_;
                    }
                    break;
                case Status::detach: // TLA:StealWork.VDeschedule
                case Status::exit:
                    // Inline deschedule without re-acquiring run_mu.
                    assert(self->next_);
                    if (busy == self && (busy = self->next_) == self) {
                        busy = nullptr;
                    }
                    if (self->next_) self->next_->prev_ = self->prev_;
                    if (self->prev_) self->prev_->next_ = self->next_;
                    self->next_ = nullptr;
                    self->prev_ = nullptr;

                    // TLA:DrainSuspended.CheckWP
                    if (status == Status::detach &&
                        self->wake_pending_.exchange(false, std::memory_order_acq_rel)) {
                        if (busy) {
                            self->next_ = busy;
                            self->prev_ = busy->prev_;
                            self->next_->prev_ = self->prev_->next_ = self;
                        } else {
                            busy = self->next_ = self->prev_ = self;
                        }
                        return;
                    }
                    break;
                default: CSP_UNREACHABLE();
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

            auto killme = status == Status::exit ? self : nullptr;
            auto killyou = reinterpret_cast<Imp *>(switch_to(*this, reinterpret_cast<intptr_t>(killme)));
            if (killyou) {
                destroy_imp(killyou);
            }

            if (!killme) {
                set_current_imp(self);
            }
        }

        // TLA:StealWork.VDoSwitch
        void do_switch(Status status) {
            auto* self = current_imp();
            // Reclaim unused stack pages before suspending.
            if (self->stk_) {
                StackPool::instance().maybe_shrink(
                    self->stk_, CSP_FRAME_ADDRESS());
            }
            Imp* target;
            {
                std::lock_guard<std::mutex> lk(current_p().run_mu);
                // Update running to the active MT so steal_work skips it.
                // (local_next sets running for the initial pick; chained
                // do_switch calls keep it current as execution moves
                // between imps.)
                current_p().running = self;
                auto& busy = current_p().busy;
                if (busy == self) {
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

    void reset_scheduler() {
        g_scheduler = default_scheduler_impl;
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
        Imp & self;
        Imp & caller;
    };

}

static void start(transfer_t t) {
#if CSP_ASAN
    // First entry into this fiber — no previous fake-stack to restore.
    __sanitizer_finish_switch_fiber(nullptr, nullptr, nullptr);
#endif
    if (current_p().save_ctx) {
        current_p().save_ctx->store(t.fctx, std::memory_order_release);
        drain_suspended(current_p().save_imp);
    }
    // Copy all data from StartData before the warmup switch, because
    // StartData lives on the spawner's stack and may be freed before
    // this imp resumes (in M:N mode, resumption happens on a
    // different OS thread after the spawner has returned).
    auto & sd = *reinterpret_cast<StartData const *>(t.data);
    auto start_f = sd.start_f;
    auto data = sd.data;
    auto * self = &sd.self;
    auto parent_dyn_ctx = sd.caller.dyn_ctx_;
    // Retain and assign parent's dynamic context BEFORE the warmup
    // switch.  After the switch the parent continues running and may
    // release its dyn_ctx (e.g. by exiting a context_scope or dying),
    // which could free the HAMT node before this imp resumes.
    if (parent_dyn_ctx) csp::internal::hamt_retain(parent_dyn_ctx);
    self->dyn_ctx_ = parent_dyn_ctx;
    set_current_imp(self);
    auto killyou_val = switch_to(sd.caller, 0);
    // After warmup switch, sd may be invalid. Use local copies only.
    set_current_imp(self);

    // In M:N mode, the resuming switch may carry a killyou pointer — a
    // dying imp that exited and chained into us via run(exit).
    // Clean it up before running our own function.
    if (auto* killyou = reinterpret_cast<Imp*>(killyou_val)) {
        destroy_imp(killyou);
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
    (void)current_p(); // Ensure current_imp() is bound before use.
    auto* self = current_imp();
    // Reclaim unused stack pages at this API boundary.
    if (self->stk_) {
        StackPool::instance().maybe_shrink(
            self->stk_, CSP_FRAME_ADDRESS());
    }
    try {
#if CSP_USE_VM_STACKS
        auto& pool = StackPool::instance();
        auto region = pool.allocate();
        auto page_sz = pool.page_size();
        auto* top = static_cast<char*>(region.base) + region.total_size;
        auto* imp = reinterpret_cast<Imp*>(top) - 1;
        assert(((uintptr_t)imp % 16) == 0);
        auto* usable_base = static_cast<char*>(region.base) + page_sz;
        auto usable_size = static_cast<size_t>((char*)imp - usable_base);
#ifdef _WIN32
        // On Windows, pass only the initially committed stack size to
        // make_fcontext.  This sets NT_TIB StackLimit (saved in the
        // fcontext at offset 0xc0) to the bottom of the committed
        // region instead of the bottom of the full MEM_RESERVE region.
        // If StackLimit points into uncommitted pages, MSVC's C++
        // exception dispatch (RtlVirtualUnwind) faults during stack
        // probing — a nested ACCESS_VIOLATION that terminates the
        // process without VEH notification.
        // The committed region is [top - kInitialCommitSize, top].
        // make_fcontext computes StackLimit as (imp - size), so the size
        // must be measured from imp (not top) to the committed bottom:
        //   size = imp - (top - kInitialCommitSize)
        //        = kInitialCommitSize - sizeof(Imp)
        auto* committed_bottom = top - std::min(
            StackPool::kInitialCommitSize,
            static_cast<size_t>(top - usable_base));
        auto committed = static_cast<size_t>((char*)imp - committed_bottom);
        auto ctx = make_fcontext(imp, committed, start);
        // make_fcontext also sets DeallocationStack (offset 0xb8) to
        // the same value as StackLimit.  Patch it to the actual bottom
        // of the reserved region so Windows stack-overflow detection
        // knows the full extent of the stack.
        *reinterpret_cast<void**>(
            static_cast<char*>(ctx) + 0xb8) = usable_base;
#else
        auto ctx = make_fcontext(imp, usable_size, start);
#endif
        new (imp) Imp(ctx, region);
#else
        // Under sanitizers: heap-allocate with stack analyzer right-sizing.
        auto region = StackPool::instance().allocate();
        size_t S = region.total_size / 16;
        auto* stk = static_cast<Imp::StackSlot*>(region.base);
        auto* imp = reinterpret_cast<Imp*>(stk + S) - 1;
        assert(((uintptr_t)imp % 16) == 0);
        auto ctx = make_fcontext(imp, (char*)imp - (char*)stk, start);
        new (imp) Imp(ctx, region);
#endif
#if CSP_TSAN
        imp->tsan_fiber_ = __tsan_create_fiber(0);
#endif

        StartData const start_data = {start_f, data, *imp, *self};
        switch_to(*imp, reinterpret_cast<intptr_t>(&start_data));
        set_current_imp(self);

        auto& rt = Runtime::instance();
        rt.live_gs.fetch_add(1, std::memory_order_relaxed);

        if (rt.mn_mode_) {
            // M:N mode: after the handshake switch_to, imp is initialized
            // and suspended but NOT on any run queue. Push it to the
            // global queue for workers to pick up and run.
            {
                std::lock_guard<std::mutex> lk(rt.global_mu);
                rt.push_to_global(imp);
            }
            rt.park_cv.notify_all();
        } else {
            // Single-P mode: run the imp on the main thread
            // (original behavior — run until it yields).
            imp->run(Status::run);
            rt.unpark_one();
        }

        return 1;
    } catch (std::exception const & e) {
        return 0;
    } catch (...) {
        return 0;
    }
}

void suspend() {
    current_imp()->suspending_.store(true, std::memory_order_release);
    do_switch(Status::detach);
    current_imp()->suspending_.store(false, std::memory_order_release);
}

int run() {
    auto& p = current_p();
    auto& rt = Runtime::instance();

    // Drain global run queue (reactor events post here in single-P mode).
    {
        std::lock_guard<std::mutex> lk(rt.global_mu);
        while (!rt.global_run_queue.empty()) {
            auto* imp = rt.global_run_queue.front();
            rt.global_run_queue.pop_front();
            imp->in_global_ = false;
            imp->schedule_local();
        }
        rt.has_global_work_.store(false, std::memory_order_release);
    }

    Imp* target = nullptr;
    {
        std::lock_guard<std::mutex> lk(p.run_mu);
        auto& busy = p.busy;
        auto* ci = current_imp();
        if (busy == ci) {
            busy = busy->next_;
        }
        if (busy != ci) {
            target = busy;
        }
    }

    if (target) {
        target->run();
    }

    {
        std::lock_guard<std::mutex> lk(p.run_mu);
        return p.busy->next_ != p.busy;
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
    vstatus(current_imp(), fmt, args);
    va_end(args);

#ifdef __APPLE__
    pthread_setname_np(getstatus(current_imp()));
#endif
}

char const * get_descr(void * thr) {
    return getfullstatus(thr ? static_cast<Imp const *>(thr) : current_imp());
}

} // namespace csp::internal
