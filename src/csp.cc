#include <csp/internal/runtime.h>
#include <csp/internal/reactor.h>
#include <csp/internal/hamt.h>
#include <csp/internal/stack_pool.h>
#include <csp/stack_analysis.h>

#ifndef _WIN32
#include <pthread.h>
#endif

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <thread>
#include <utility>

#include <stdlib.h>

static void main_loop_scheduler() {
    auto& rt = csp::detail::Runtime::instance();
    rt.main_loop();
    csp::detail::Reactor::instance().shutdown();
}

static std::function<void()> g_scheduler = main_loop_scheduler;

// 🎯T3.4.4: per-entry-function stack high-water table. Process-global,
// in-memory only — resets on process restart. The acceptance criterion
// requires no persistence and a 50% margin applied at the consumer; this
// store keeps the raw maximum so future spawns can decide independently
// how much headroom to add.
//
// Synchronisation: a mutex + std::unordered_map is sufficient because
// updates happen at suspend checkpoints (already a relatively rare
// boundary compared to runtime hot paths) and the table never grows
// beyond the count of distinct entry functions spawned in the process —
// typically a small set. Lock contention is not a concern at the
// expected scale.
namespace {
    std::mutex g_high_water_mu;
    std::unordered_map<csp::internal::EntryFn, size_t> g_high_water;
}

namespace csp::detail {

void record_stack_high_water(::csp::internal::EntryFn fn, size_t depth_bytes) {
    if (!fn) return;
    std::lock_guard lk(g_high_water_mu);
    auto& slot = g_high_water[fn];
    if (depth_bytes > slot) slot = depth_bytes;
}

size_t get_stack_high_water(::csp::internal::EntryFn fn) {
    if (!fn) return 0;
    std::lock_guard lk(g_high_water_mu);
    auto it = g_high_water.find(fn);
    return it == g_high_water.end() ? 0 : it->second;
}

}  // namespace csp::detail

#if CSP_STACK_PAINT
// 🎯T52.2: per-entry-function true-peak table (painted watermark). Same
// shape and synchronisation rationale as the high-water table above; the
// writers here are destroy_imp scans (once per imp exit) rather than
// suspend checkpoints.
namespace {
    std::mutex g_true_peak_mu;
    std::unordered_map<csp::internal::EntryFn, size_t> g_true_peak;
}

namespace csp::detail {

void record_stack_true_peak(::csp::internal::EntryFn fn, size_t peak_bytes) {
    if (!fn) return;
    std::lock_guard lk(g_true_peak_mu);
    auto& slot = g_true_peak[fn];
    if (peak_bytes > slot) slot = peak_bytes;
}

size_t get_stack_true_peak(::csp::internal::EntryFn fn) {
    if (!fn) return 0;
    std::lock_guard lk(g_true_peak_mu);
    auto it = g_true_peak.find(fn);
    return it == g_true_peak.end() ? 0 : it->second;
}

// Sentinel for the stack-painting watermark. Painting happens per HANDOUT
// (not per slot creation) in spawn(), so slot reuse always starts from a
// fully repainted region; the scan happens in destroy_imp() before the slot
// returns to the free list. 0xA5 is the classic FreeRTOS pattern — an
// 8-byte word of it is vanishingly unlikely as legitimate frame data, and
// the scan below matches word-wise, so a single stray 0xA5 byte inside the
// deepest frame cannot shift the low-water mark by more than 8 bytes.
constexpr unsigned char kStackPaintByte = 0xA5;

// Paint [guard end, paint_end) with the sentinel. Called with paint_end =
// the imp's entry SP (the Imp control block address at the top of the
// slot), BEFORE make_fcontext writes the boot record — the boot record and
// everything the imp subsequently touches count as "used".
static void paint_stack(StackRegion const& region, void const* paint_end) {
    auto* lo = static_cast<char*>(region.overflow_limit);
    auto* hi = static_cast<char*>(const_cast<void*>(paint_end));
    if (lo && lo < hi) {
        memset(lo, kStackPaintByte, static_cast<size_t>(hi - lo));
    }
}

// Forward scan from the guard end for the first unpainted byte — O(unused
// bytes), fine for audit builds. Returns the true peak in bytes measured
// from `top` (the imp's entry SP), i.e. the same base the checkpoint
// high-water uses.
static size_t scan_stack_true_peak(StackRegion const& region, void const* top) {
    auto const* lo = static_cast<unsigned char const*>(region.overflow_limit);
    auto const* hi = static_cast<unsigned char const*>(top);
    if (!lo || lo >= hi) return 0;
    uint64_t pattern;
    memset(&pattern, kStackPaintByte, sizeof(pattern));
    auto const* p = lo;
    while (p < hi && (reinterpret_cast<uintptr_t>(p) & 7) != 0) {
        if (*p != kStackPaintByte) return static_cast<size_t>(hi - p);
        ++p;
    }
    while (p + 8 <= hi) {
        uint64_t w;
        memcpy(&w, p, sizeof(w));
        if (w != pattern) break;
        p += 8;
    }
    while (p < hi && *p == kStackPaintByte) ++p;
    return static_cast<size_t>(hi - p);
}

}  // namespace csp::detail
#endif  // CSP_STACK_PAINT

#ifdef CSP_ANALYSE_STACKS
// 🎯T52.2: corpus metric counters — total spawns vs spawns that landed a
// Small slot, dumped at exit when CSP_STACK_STATS is set (same idiom as
// CSP_PROC_STATS in runtime.cpp). Compiled out with the analyser; the
// consumer is scripts/stack_metric.py, which aggregates these lines across
// the test-suite + examples corpus and ratchets the Small-slot rate.
namespace {
    std::atomic<size_t> g_spawn_total{0};
    std::atomic<size_t> g_spawn_small{0};

    void print_stack_stats() {
        if (std::getenv("CSP_STACK_STATS") == nullptr) return;
        size_t total = g_spawn_total.load(std::memory_order_relaxed);
        size_t small = g_spawn_small.load(std::memory_order_relaxed);
        // Bytes saved vs all-Default = Small spawns × the per-slot
        // address-space footprint difference (physical pages are demand-
        // committed/madvised either way; the footprint is the metric).
        size_t saved = small *
            (csp::detail::StackPool::slot_footprint_bytes(
                 csp::detail::StackClass::Default) -
             csp::detail::StackPool::slot_footprint_bytes(
                 csp::detail::StackClass::Small));
        fprintf(stderr,
                "CSP_STACK_SPAWNS_TOTAL=%zu CSP_STACK_SPAWNS_SMALL=%zu "
                "CSP_STACK_BYTES_SAVED=%zu\n",
                total, small, saved);
    }

    void note_spawn_slot_class(bool small) {
        [[maybe_unused]] static bool registered =
            (std::atexit(print_stack_stats), true);
        g_spawn_total.fetch_add(1, std::memory_order_relaxed);
        if (small) g_spawn_small.fetch_add(1, std::memory_order_relaxed);
    }
}
#endif  // CSP_ANALYSE_STACKS


namespace csp {

    namespace detail {

        [[noreturn]] void throw_no_imp(const char* what) {
            throw error(std::string(what) +
                " requires an imp context — "
                "use csp::run() or csp::spawn() to enter the runtime");
        }

        struct alignas(16) align_16 {
            char c[16];
        };

        static void vstatus(Imp * imp, char const * msg, va_list args) {
            char * buf = imp->status_;
            int len = sizeof(imp->status_);
            int n = snprintf(buf, len, "§%zu ", imp->id_);
            vsnprintf(buf += n, len -= n, msg, args);
        }

        // After a context save completes, close the suspended imp's
        // suspend window with ONE atomic exchange on its state word
        // (🎯T34 O2). Seeing SUSP_WAKE means a waker deferred to us —
        // queue the imp now. The exchange is what makes the drain and
        // schedule()'s CAS linearizable without the global mutex: a
        // split load-then-clear would let the waker's CAS land in
        // between and be erased (see DrainSuspended_Bug.tla).
        static void drain_suspended(Imp* suspended) {
            // TLA:DrainSuspended.Drain
            auto old = suspended->suspend_state_.exchange(
                Imp::SUSP_IDLE, std::memory_order_acq_rel);
            if (old != Imp::SUSP_WAKE) {
                return;
            }
            // Deferred wake: the waker returned after its CAS; queuing
            // is our job. Same placement path as schedule()'s IDLE arm
            // so the drain also gets wake-to-local (🎯T37) — previously
            // this always hit global_mu + unpark_one, which reintroduced
            // the park/steal thrash on every close-race rendezvous.
            if (suspended->qs_entered_
                && suspended->qs_sleeping_.exchange(false, std::memory_order_acq_rel)) {
                suspended->qs_->enter();
            }
            // Claim placement against late wakers that observed
            // SUSP_IDLE after our exchange above. TLA:PlacementClaim.Claim
            if (suspended->placed_.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            suspended->place_on_run_queue();
        }

        // self must be current_imp() — passed through to spare the
        // (deliberately non-inline) TLS accessor another call.
        static intptr_t switch_to(Imp & target, intptr_t data, Imp * self) {
            // Acquire-load ctx_ to synchronize with the release-store
            // that saved the target's context on a (possibly different)
            // OS thread.  This ensures the saved register data on the
            // target's stack is visible to us before we jump.
            auto ctx = target.ctx_.load(std::memory_order_acquire);
            {
                auto& p = current_p();
                p.save_ctx = &self->ctx_;
                p.save_imp = self;
            }
#if CSP_ASAN
            __sanitizer_start_switch_fiber(
                &self->asan_fake_stack_,
                target.stk_.base, target.stk_.total_size);
#endif
#if CSP_TSAN
            __tsan_switch_to_fiber(target.tsan_fiber_, 0);
#endif
            auto t = csp_jump(ctx, (void *)data);
#if CSP_ASAN
            __sanitizer_finish_switch_fiber(
                self->asan_fake_stack_, nullptr, nullptr);
#endif
            // Release-store our caller's saved SP so that any thread
            // that later acquire-loads ctx_ will also see the register
            // data that jump_fcontext wrote to the caller's stack.
            // Re-resolve the processor: we may have resumed on a
            // different OS thread than the one that jumped.
            auto& p_after = current_p();
            p_after.save_ctx->store(t.fctx, std::memory_order_release);
            drain_suspended(p_after.save_imp);
            return (intptr_t)t.data;
        }

        Imp::Imp(fcontext_t ctx, StackRegion stk) : ctx_(ctx), stk_(stk) {
            prev_ = next_ = nullptr;
            snprintf(status_, sizeof(status_), "§%zu", id_);
        }

        Imp::Imp() : Imp(nullptr, {}) {
            prev_ = next_ = this;
            // The synthetic main imp is born linked into its P's ring.
            placed_.store(true, std::memory_order_relaxed);
            snprintf(status_, sizeof(status_), "§main");
        }

        Imp::~Imp() {
            if (dyn_ctx_) csp::internal::hamt_release(dyn_ctx_);
            delete local_ctx_;
        }

        void Imp::schedule_local() {
            auto& p = current_p();
            std::lock_guard lk(p.run_mu);
            if (next_) {
                return;
            }
            auto& busy = p.busy;
            if (busy) {
                next_ = busy;
                prev_ = busy->prev_;
                next_->prev_ = prev_->next_ = this;
            } else {
                busy = next_ = prev_ = this;
            }
        }

        void Imp::place_on_run_queue() {
            // Caller has claimed placed_ and asserts !next_ && !in_global_.
            auto& rt = Runtime::instance();
            assert(placed_.load(std::memory_order_relaxed));
            assert(!next_ && !in_global_);

            // 🎯T34 O1 wake-to-local: a wake issued from a worker P
            // whose local queue has no other waiting imp hands the
            // woken imp to that P directly — the very next do_switch
            // runs it, with no futex wake and no cross-core migration
            // (the dominant rendezvous cost in paper 33). run_mu alone
            // suffices: the placement claim above makes this placer
            // exclusive. The queued imp stays stealable, and the
            // watchdog rescues it if this P wedges in a long compute
            // stretch (100 ms backstop). Excluded: P0 (its thread runs
            // main_loop, never imps) and non-P threads (reactor,
            // blocking pool).
            // TLA:StealWork.WAcquireRunMu TLA:StealWork.WPushLocal
            // TLA:PlacementClaim.Insert
            if (has_processor()) {
                if (auto& p = current_p(); p.id != 0) {
                    // Fairness budget: after kLocalWakeBudget consecutive
                    // local wakes, PULL one batch of global work into this
                    // P's ring (take_from_global below). A hot rendezvous
                    // pair otherwise monopolizes its P while spawned imps
                    // starve in the global queue — the flat_map balloon:
                    // merge+producer looped locally, sub-stream imps never
                    // ran, the input arm stayed the only ready one, and the
                    // merge's vector alt grew by one channel per iteration
                    // (measured: 5001 arms after 5000 consecutive spins).
                    // Pulling (rather than spilling our peer to the global
                    // queue) keeps the pair's locality: a spill design
                    // measured +170 ns/op at 16 procs from pair migration.
                    // 32 keeps a ballooned alt's lock_all under TSan's
                    // 64-entry deadlock-detector table as defense in depth.
                    constexpr int kLocalWakeBudget = 32;
                    bool queued = false;
                    bool pull = false;
                    {
                        std::lock_guard plk(p.run_mu);
                        bool has_waiting = p.ring_has_waiting();
                        if (!has_waiting
                            && ++p.local_wake_streak_ >= kLocalWakeBudget) {
                            p.local_wake_streak_ = 0;
                            pull = true;
                        }
                        if (!has_waiting) {
                            if (p.busy) {
                                next_ = p.busy;
                                prev_ = p.busy->prev_;
                                next_->prev_ = prev_->next_ = this;
                            } else {
                                p.busy = next_ = prev_ = this;
                            }
                            queued = true;
                        }
                        // else: local queue already has waiting work —
                        // spill to the global queue so parked workers
                        // share the load.
                    }
                    if (queued) {
                        if (pull
                            && rt.has_global_work_.load(std::memory_order_acquire)) {
                            // TLA:StealWork.TkAcquireGlobal — same
                            // take path a worker uses; global work lands
                            // in this ring and runs when the pair blocks.
                            rt.take_from_global(p);
                        }
                        return;  // no unpark: this P runs it next
                    }
                }
            }
            // Spill path: global queue + worker wake.
            // TLA:DrainSuspended.WakerPush TLA:StealWork.WStartSchedule
            // TLA:StealWork.WAcquireLock TLA:StealWork.WPush
            {
                std::lock_guard lk(rt.global_mu);
                rt.push_to_global(this);
            }
            rt.unpark_one();
        }

        void Imp::schedule() {
            // Suspend-window handshake first, lock-free: if the imp is
            // in the unlock_all→do_switch window, it's still running
            // and can't be safely pushed to a queue. One CAS defers
            // the wake to CheckWP (early) or drain_suspended (after
            // the context save). TLA:DrainSuspended.WakerCAS
            for (;;) {
                uint32_t s = suspend_state_.load(std::memory_order_acquire);
                if (s == SUSP_PENDING) {
                    if (suspend_state_.compare_exchange_weak(
                            s, SUSP_WAKE,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        return;
                    }
                    continue;  // raced with the drain or a spurious failure
                }
                if (s == SUSP_WAKE) {
                    return;  // wake already pending
                }
                break;  // SUSP_IDLE: fully suspended (or never was)
            }

            // Claim placement — exactly one racing placer (duplicate
            // wakers, deferred-wake drain) inserts the imp; a TRUE
            // result means it is already queued, running, or another
            // placer is committed. Replaces the global_mu-serialized
            // next_/in_global_ checks: the wake path no longer touches
            // the global lock at all unless it spills.
            // TLA:PlacementClaim.Claim
            if (placed_.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            place_on_run_queue();
        }

        void Imp::make_runnable() {
            // Enter quiescence scope at schedule time (closes the gap
            // between leave and resume). Atomic exchange prevents
            // double-enter if multiple threads schedule the same imp.
            if (qs_entered_ && qs_sleeping_.exchange(false, std::memory_order_acq_rel)) {
                qs_->enter();
            }
            schedule();
        }

        static void destroy_imp(Imp* imp) {
#if CSP_TSAN
            if (imp->tsan_fiber_) __tsan_destroy_fiber(imp->tsan_fiber_);
#endif
#if CSP_STACK_PAINT
            // 🎯T52.2: scan the painted slot for its low-water mark BEFORE
            // the slot returns to the free list (release() below madvises
            // the usable pages away). destroy_imp always runs on the OS
            // thread that just switched away from the dying imp, so the
            // scan is ordered after every frame the imp ever wrote.
            if (imp->stk_ && imp->entry_fn_ && imp->entry_sp_) {
                record_stack_true_peak(
                    imp->entry_fn_,
                    scan_stack_true_peak(imp->stk_, imp->entry_sp_));
            }
#endif
            bool was_daemon = imp->daemon_;
            auto region = imp->stk_;
            imp->~Imp();
            StackPool::instance().release(region);
            auto& rt = Runtime::instance();
            if (was_daemon) rt.daemon_gs.fetch_sub(1, std::memory_order_relaxed);
            auto live = rt.live_gs.fetch_sub(1, std::memory_order_acq_rel);
            auto daemon = rt.daemon_gs.load(std::memory_order_acquire);
            if (live - 1 <= daemon) {
                // All non-daemon imps are done.
                rt.notify_watchers();
            }
        }

        // Worker dispatch: resume a queued imp from local_next(). The
        // dispatching context is the P's synthetic main (or a resumed
        // imp's continuation), which stays linked in the ring — only
        // advance busy past it. Detach/exit departures all go through
        // do_switch, which owns the sole copy of the CheckWP early-wake
        // protocol (🎯T49; the arm here was a stale duplicate: the one
        // caller, worker_loop, always passed Status::sleep).
        void Imp::run() {
            auto& p = current_p();
            auto& busy = p.busy;
            auto self = current_imp();
            if (this == self) {
                throw error(
                    "channel operation attempted from main() — "
                    "CSP operations must run inside spawn()");
            }

            // Manipulate run queue under lock, but release before context switch.
            {
                std::lock_guard lk(p.run_mu);

                if (self == busy) {
                    busy = busy->next_;
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

            auto killyou = reinterpret_cast<Imp *>(switch_to(*this, 0, self));
            if (killyou) {
                destroy_imp(killyou);
            }

            set_current_imp(self);
        }

        // TLA:StealWork.VDoSwitch
        void do_switch(Status status) {
            do_switch(status, current_imp());
        }

        void do_switch(Status status, Imp * self) {
            if (self->qs_entered_) {
                self->qs_sleeping_.store(true, std::memory_order_release);
                self->qs_->leave();
            }
            // Reclaim unused stack pages before suspending.
            if (self->stk_) {
                auto* fp = CSP_FRAME_ADDRESS();
                check_stack_overflow(self, fp);
#ifdef CSP_ANALYSE_STACKS
                // 🎯T3.4.4: profile this suspend's depth into the per-entry
                // high-water table. The recorded value refines the analyser's
                // indirect_call_budget on the next spawn() of the same entry
                // function. It never selects the slot class directly (🎯T33):
                // a checkpoint sample is not a sound upper bound, so it must
                // not gate the Small slot. Compiled out with the analyser
                // (its only consumer): a mutex + hash lookup per suspend is
                // pure waste otherwise (🎯T35 profiling).
                if (self->entry_fn_ && self->entry_sp_) {
                    auto* top = static_cast<char*>(self->entry_sp_);
                    auto* cur = static_cast<char*>(fp);
                    if (top > cur) {
                        record_stack_high_water(
                            self->entry_fn_, static_cast<size_t>(top - cur));
                    }
                }
#endif
                StackPool::instance().maybe_shrink(self->stk_, fp);
            }
            auto& p = current_p();
            // A synthetic main imp only calls do_switch when CSP
            // operations are attempted outside spawn() — reject before
            // touching the run queue (the merged section below would
            // otherwise delink it from its own ring first).
            if (self == &p.main) {
                throw error(
                    "channel operation attempted from main() — "
                    "CSP operations must run inside spawn()");
            }
            Imp* target;
            bool stay = false;  // CheckWP took an early wake — no switch
            {
                std::lock_guard lk(p.run_mu);
                // Update running to the active MT so steal_work skips it.
                // (local_next sets running for the initial pick; chained
                // do_switch calls keep it current as execution moves
                // between imps.)
                p.running = self;
                auto& busy = p.busy;
                if (busy == self) {
                    busy = busy->next_;
                }
                // Self's departure bookkeeping — merged from Imp::run()
                // (🎯T35): one run_mu section per switch instead of two.
                switch (status) {
                case Status::sleep:
                    // busy already advanced past self; it stays linked.
                    break;
                case Status::detach: // TLA:StealWork.VDeschedule
                case Status::exit:
                    // Inline deschedule.
                    assert(self->next_);
                    if (busy == self && (busy = self->next_) == self) {
                        busy = nullptr;
                    }
                    if (self->next_) self->next_->prev_ = self->prev_;
                    if (self->prev_) self->prev_->next_ = self->next_;
                    self->next_ = nullptr;
                    self->prev_ = nullptr;

                    // TLA:DrainSuspended.CheckWP — early wake: a waker
                    // CASed to SUSP_WAKE before our context save. Take
                    // the wake here (CAS back to IDLE), re-add to the
                    // local queue, and skip the switch entirely.
                    if (uint32_t wake = Imp::SUSP_WAKE;
                        status == Status::detach &&
                        self->suspend_state_.compare_exchange_strong(
                            wake, Imp::SUSP_IDLE,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        if (busy) {
                            self->next_ = busy;
                            self->prev_ = busy->prev_;
                            self->next_->prev_ = self->prev_->next_ = self;
                        } else {
                            busy = self->next_ = self->prev_ = self;
                        }
                        stay = true;
                        break;
                    }
                    // Committed to switching out: release the placement
                    // claim. Ordered after CheckWP so an early-woken imp
                    // (which stays queued) never exposes placed_ == FALSE.
                    // TLA:PlacementClaim
                    self->placed_.store(false, std::memory_order_release);
                    break;
                default: CSP_UNREACHABLE();
                }
                target = busy;
                // The ring always contains this P's sentinel (misuse was
                // rejected above), so a target exists and is not self.
                // Checked under run_mu: target's links are shared with
                // concurrent thieves.
                assert(stay || (target && target != self && target->next_));
            }
            if (!stay) {
                auto killme = status == Status::exit ? self : nullptr;
                auto killyou = reinterpret_cast<Imp*>(switch_to(
                    *target, reinterpret_cast<intptr_t>(killme), self));
                if (killyou) {
                    destroy_imp(killyou);
                }
                if (!killme) {
                    set_current_imp(self);
                }
            }
            // Re-enter scope if we left it (yield path). For scheduled
            // imps, make_runnable already entered — exchange returns false.
            // self is still this imp after resume; only the TLS slot must
            // not be cached across the switch, not the Imp* value.
            if (self->qs_entered_
                && self->qs_sleeping_.exchange(false, std::memory_order_acq_rel)) {
                self->qs_->enter();
            }
        }

    }

    void set_scheduler(std::function<void()> scheduler) {
        g_scheduler = std::move(scheduler);
    }

    void reset_scheduler() {
        g_scheduler = main_loop_scheduler;
    }

    void await_completion() {
        detail::current_p();
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
    auto killyou_val = switch_to(sd.caller, 0, self);
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
    } catch (std::exception const &) {
    } catch (...) {
    }
    do_switch(Status::exit);
};

namespace csp::internal {

int spawn(EntryFn start_f, void * data, bool daemon) {
    (void)current_p(); // Ensure current_imp() is bound before use.
    auto* self = current_imp();
    // Reclaim unused stack pages at this API boundary.
    if (self->stk_) {
        auto* fp = CSP_FRAME_ADDRESS();
        check_stack_overflow(self, fp);
        StackPool::instance().maybe_shrink(self->stk_, fp);
    }

    // Pick a slot class for the new imp's stack. Default everywhere unless
    // the analyser is enabled (CSP_ANALYSE_STACKS) and the cached analyser
    // estimate is a sound upper bound (is_exact) that fits the Small slot
    // with margin (🎯T3.4.1).
    //
    // 🎯T33: the Small slot must NEVER be chosen from the checkpoint-sampled
    // per-entry high-water table. That value is sampled only at do_switch
    // checkpoints, so a helper that grows and unwinds between two channel ops
    // is never observed; it is also keyed per EntryFn and shared across all
    // instances, so a shallow instance's small sample would gate a deep
    // instance's allocation. Arena Small slots have only a 4 KB software
    // guard (no PROT_NONE page), so a mis-sized slot silently corrupts a
    // neighbouring imp. The profile therefore only refines the analyser's
    // indirect_call_budget for the never-shrinking Default slot (the 🎯T3.4.4
    // invariant: the profile improves the magnitude of the inexact fallback,
    // it does not down-size the slot class). See docs/audit/fable-2026-07.md
    // (F1); the design rationale is paper 08 §7 (analyser must never
    // underestimate).
    StackClass slot_cls = StackClass::Default;
#ifdef CSP_ANALYSE_STACKS
    if (constexpr size_t kSmallUsable = StackPool::small_slot_usable_bytes();
        kSmallUsable > 0) {
        constexpr size_t kHeadroomFloor = 2 * 1024;

        ::csp::stack_analysis_options opts;
        // Profile-derived budget refines the magnitude of OP_BUDGET results
        // when the walker can't resolve an indirect call. Recorded
        // high-water + 50% margin replaces the flat 2 KB default; this is the
        // indirect_call_budget surface promised by 🎯T3.4.4 (eval-time
        // consumption keeps the program cache budget-agnostic). It never
        // selects the slot class — only sharpens the analyser's own estimate,
        // whose is_exact flag remains the sole gate below.
        if (size_t hw = ::csp::detail::get_stack_high_water(start_f); hw > 0) {
            size_t refined = hw + hw / 2;
            if (refined > opts.indirect_call_budget) {
                opts.indirect_call_budget = refined;
            }
        }
        auto sa = ::csp::analyze_stack_depth_cached(
            reinterpret_cast<const void*>(start_f), data, opts);
        // 2× headroom + 2 KB absolute floor (ABI spill / signal frame).
        size_t needed = sa.max_depth * 2 + kHeadroomFloor + sizeof(Imp);
        // Small is gated strictly on a sound static upper bound. An inexact
        // analyser result (the common type-erased csp::spawn(lambda) case)
        // keeps the Default slot no matter what the profile observed.
        if (sa.is_exact && needed <= kSmallUsable) {
            slot_cls = StackClass::Small;
        }
    }
    // 🎯T52.2 corpus metric: count every spawn and which slot class it
    // landed (dumped at exit under CSP_STACK_STATS).
    note_spawn_slot_class(slot_cls == StackClass::Small);
#endif

    try {
#if CSP_USE_VM_STACKS
        auto& pool = StackPool::instance();
        auto region = pool.allocate(slot_cls);
        auto page_sz = pool.page_size();
        auto* top = static_cast<char*>(region.base) + region.total_size;
        auto* imp = reinterpret_cast<Imp*>(top) - 1;
        assert(((uintptr_t)imp % 16) == 0);
        auto* usable_base = static_cast<char*>(region.base) + page_sz;
        [[maybe_unused]] auto usable_size = static_cast<size_t>((char*)imp - usable_base);
#if CSP_STACK_PAINT
        // 🎯T52.2: paint the handed-out slot with the watermark sentinel.
        // Per handout, not per slot creation — a reused slot still carries
        // the previous occupant's frames. Must precede make_fcontext (the
        // boot record it writes below `imp` counts as used bytes) and the
        // warmup switch (which runs the new fiber's first frames).
        paint_stack(region, imp);
#endif
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
        auto ctx = csp_make(imp, usable_size, start);
#endif
        new (imp) Imp(ctx, region);
        imp->stack_overflow_limit_ = region.overflow_limit;
        // 🎯T3.4.4: stash entry function + stack top so suspend
        // checkpoints can compute depth = (entry_sp - current_sp).
        imp->entry_fn_ = start_f;
        imp->entry_sp_ = static_cast<void*>(imp);
#else
        // Under sanitizers: heap-allocate with stack analyzer right-sizing.
        auto region = StackPool::instance().allocate(slot_cls);
        size_t S = region.total_size / 16;
        auto* stk = static_cast<Imp::StackSlot*>(region.base);
        auto* imp = reinterpret_cast<Imp*>(stk + S) - 1;
        assert(((uintptr_t)imp % 16) == 0);
        auto ctx = csp_make(imp, (char*)imp - (char*)stk, start);
        new (imp) Imp(ctx, region);
        imp->entry_fn_ = start_f;
        imp->entry_sp_ = static_cast<void*>(imp);
#endif
#if CSP_TSAN
        imp->tsan_fiber_ = __tsan_create_fiber(0);
#endif

        StartData const start_data = {start_f, data, *imp, *self};
        switch_to(*imp, reinterpret_cast<intptr_t>(&start_data), self);
        set_current_imp(self);

        // Inherit quiescence scope from parent.
        imp->qs_ = self->qs_;
        if (imp->qs_) {
            imp->qs_->enter();
            imp->qs_entered_ = true;
        }

        auto& rt = Runtime::instance();
        if (daemon) {
            imp->daemon_ = true;
            rt.daemon_gs.fetch_add(1, std::memory_order_relaxed);
        }
        rt.live_gs.fetch_add(1, std::memory_order_relaxed);

        // Push to the global queue for workers to pick up and run.
        // The spawner is the sole owner pre-publication — a plain
        // placement store suffices (no racing placers yet).
        imp->placed_.store(true, std::memory_order_relaxed);
        {
            std::lock_guard lk(rt.global_mu);
            rt.push_to_global(imp);
        }
        // Wake one sleeping worker Note (not park_cv — that is only for
        // completion/quiescence watchers, gated separately).
        rt.unpark_one();

        return 1;
    } catch (std::exception const &) {
        return 0;
    } catch (...) {
        return 0;
    }
}

void suspend() {
    // TLA:DrainSuspended.BeginSuspend — the state resets to SUSP_IDLE
    // via CheckWP (early wake) or drain_suspended (context switch).
    auto * const self = current_imp();
    self->suspend_state_.store(Imp::SUSP_PENDING, std::memory_order_release);
    do_switch(Status::detach, self);
}

int run() {
    auto& rt = Runtime::instance();
    auto user_done = [&] {
        return rt.live_gs.load(std::memory_order_acquire)
            <= rt.daemon_gs.load(std::memory_order_acquire);
    };
    if (user_done()) return 0;
    // Wait for either completion or quiescence (all workers parked +
    // no global work + no pending signals = deadlock or external
    // action needed).
    rt.park_wait([&] {
        if (user_done()) return true;
        // Check quiescence: all workers parked, no global work, no signals.
        if (rt.has_global_work_.load(std::memory_order_acquire)) return false;
        if (csp::detail::Reactor::instance().has_pending_signals()) return false;
        int np = rt.num_procs_.load(std::memory_order_acquire);
        for (int i = 1; i < np; ++i) {
            if (rt.procs[i] && rt.procs[i]->alive.load(std::memory_order_acquire)
                && !rt.procs[i]->parked.load(std::memory_order_acquire)) {
                return false;  // a worker is still active
            }
        }
        return true;  // quiescent — deadlock or done
    }, /*quiescence=*/true);
    return 0;
}

void await_idle() {
    // Wait for ALL imps (including daemons) to exit.
    // Used by test cleanup after killing daemon handlers.
    auto& rt = Runtime::instance();
    rt.park_wait([&rt] {
        return rt.live_gs.load(std::memory_order_acquire) == 0;
    });
}

} // namespace csp::internal

void csp::quiescence_scope::bind() {
    auto* imp = detail::current_imp();
    imp->qs_ = this;
    imp->qs_entered_ = true;
    enter();
}

namespace csp::internal {

void await_quiescent() {
    // Wait until all workers are parked (no runnable work anywhere).
    // At this point every live imp has registered its channel/timer
    // waiters and yielded — the system is in a deterministic state.
    auto& rt = Runtime::instance();
    rt.park_wait([&rt] {
        if (rt.has_global_work_.load(std::memory_order_acquire)) return false;
        if (csp::detail::Reactor::instance().has_pending_signals()) return false;
        int np = rt.num_procs_.load(std::memory_order_acquire);
        for (int i = 1; i < np; ++i) {
            if (rt.procs[i] && rt.procs[i]->alive.load(std::memory_order_acquire)
                && !rt.procs[i]->parked.load(std::memory_order_acquire)) {
                return false;
            }
        }
        return true;
    }, /*quiescence=*/true);
}

void yield() {
    bool should_switch;
    {
        auto& p = current_p();
        std::lock_guard lk(p.run_mu);
        should_switch = p.busy->next_ != p.busy;
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
