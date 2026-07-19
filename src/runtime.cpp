#include <csp/internal/runtime.h>
#include <csp/csp.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <csignal>
#endif

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>

namespace csp {

    namespace detail {

        static void set_thread_name(int id) {
            char name[16];
            snprintf(name, sizeof(name), "csp-%d", id);
#ifdef _WIN32
            // SetThreadDescription expects wide string
            wchar_t wname[16];
            mbstowcs(wname, name, 16);
            SetThreadDescription(GetCurrentThread(), wname);
#elif defined(__APPLE__)
            pthread_setname_np(name);
#else
            pthread_setname_np(pthread_self(), name);
#endif
        }

        // 🎯T29 repro-loop instrumentation: process-global high-water mark
        // for the processor pool, to correlate slow/hung dist-TSan runs with
        // watchdog-driven add_processor() churn (paper 32, hypothesis C1).
        // Not part of the public API — dumped to stderr only when
        // CSP_PROC_STATS is set, via a single atexit hook registered once
        // from Runtime::init().
        static std::atomic<int> g_procs_high_water{0};

        static void note_procs_high_water(int n) {
            int cur = g_procs_high_water.load(std::memory_order_relaxed);
            while (n > cur && !g_procs_high_water.compare_exchange_weak(
                                   cur, n, std::memory_order_relaxed,
                                   std::memory_order_relaxed)) {
            }
        }

        static void print_procs_high_water() {
            if (std::getenv("CSP_PROC_STATS") != nullptr) {
                fprintf(stderr, "CSP_PROC_HIGH_WATER=%d CSP_HW_CONCURRENCY=%u\n",
                        g_procs_high_water.load(std::memory_order_relaxed),
                        std::thread::hardware_concurrency());
            }
        }

        static Runtime g_runtime;

        Runtime& Runtime::instance() {
            return g_runtime;
        }

        void Runtime::init(int num_procs) {
#ifndef _WIN32
            // Ignore SIGPIPE process-wide.  On macOS, per-fd
            // F_SETNOSIGPIPE handles this, but Linux lacks that fcntl.
            // Writing to a closed pipe/socket must return EPIPE, not
            // kill the process.
            ::signal(SIGPIPE, SIG_IGN);
#endif

            // Shut down any previous state.
            if (!procs.empty()) {
                shutdown();
            }

            stopping.store(false, std::memory_order_release);
            has_global_work_.store(false, std::memory_order_release);
            live_gs.store(0, std::memory_order_release);
            // 🎯T36 / paper 34: daemon_gs must share the accounting epoch
            // with live_gs. A daemon abandoned by shutdown_runtime (workers
            // joined without destroy_imp) otherwise leaves daemon_gs elevated
            // forever, so main_loop's `live_gs <= daemon_gs` can fire while
            // non-daemon imps still run (premature schedule return → straggler
            // throw → spawn_entry std::terminate).
            daemon_gs.store(0, std::memory_order_release);

            {
                std::lock_guard lk(global_mu);
                global_run_queue.clear();
            }

            if (num_procs <= 0) {
                num_procs = std::max(2, (int)std::thread::hardware_concurrency());
            }
            // Always M:N: at least 2 procs so workers can process
            // imps while the main thread parks in main_loop().
            if (num_procs < 2) num_procs = 2;

            initial_procs_ = num_procs;
            max_procs_ = std::max(num_procs, (int)std::thread::hardware_concurrency() * 4);

            note_procs_high_water(num_procs);
            {
                static std::atomic<bool> atexit_registered{false};
                bool expected = false;
                if (atexit_registered.compare_exchange_strong(
                        expected, true, std::memory_order_relaxed)) {
                    std::atexit(print_procs_high_water);
                }
            }

            // Pre-reserve to max_procs_ so the vector never reallocates.
            // This lets steal_work and take_from_global read procs[i]
            // safely using num_procs_.load() as the bound.
            procs.clear();
            procs.resize(max_procs_);
            for (int i = 0; i < num_procs; ++i) {
                procs[i] = std::make_unique<Processor>(i);
            }
            num_procs_.store(num_procs, std::memory_order_release);
            bind_processor(procs[0].get());

            for (int i = 1; i < num_procs; ++i) {
                procs[i]->worker = std::thread([this, i] {
                    set_thread_name(i);
                    bind_processor(procs[i].get());
                    worker_loop();
                });
            }

            if (num_procs > 1) {
                watchdog_ = std::thread([this] { watchdog_loop(); });
            }
        }

        void Runtime::shutdown() {
            stopping.store(true, std::memory_order_release); // TLA:PerWorkerWake.ShutdownSetFlag

            // Wake every worker via its per-worker Note. Workers that are
            // sleeping (Note::SLEEPING) get an immediate futex wake; awake
            // workers get the FLAGGED sentinel so they skip the next sleep
            // and check stopping immediately.
            //
            // We also notify main_loop / quiescent_loop (which still use park_cv).
            //
            // Re-read num_procs_ each iteration to capture any surplus workers
            // that the watchdog added after stopping was set. Stop when the
            // count is stable (watchdog has also exited by then).
            {
                int prev_n = 0;
                for (;;) {
                    int n = num_procs_.load(std::memory_order_acquire);
                    for (int i = prev_n > 0 ? prev_n : 1; i < n; ++i) {
                        if (procs[i] && procs[i]->alive.load(std::memory_order_acquire)) {
                            procs[i]->note.wake(); // TLA:PerWorkerWake.ShutdownWakeAll
                        }
                    }
                    if (n == prev_n) break;  // Stable: no new procs added.
                    prev_n = n;
                    // Small yield to let the watchdog thread see stopping=true
                    // and stop adding new procs.
                    std::this_thread::yield();
                }
            }
            // Synchronize with main_loop / run() / quiescent_loop / await_idle(),
            // all of which wait on park_cv. Acquire-release park_mu before
            // notifying so any waiter that has evaluated the predicate as false
            // (but hasn't entered cv.wait yet) will have entered cv.wait before
            // we notify. This prevents lost wakeups. The empty critical
            // section + broadcast live inside notify_watchers().
            // TLA:WorkerParking.ShutdownAcquireMu TLA:WorkerParking.ShutdownReleaseMu
            notify_watchers(); // TLA:WorkerParking.ShutdownNotify

            if (watchdog_.joinable()) {
                watchdog_.join();
            }

            // After watchdog joins, no more procs can be added.
            // Wake any remaining sleepers (e.g., surplus workers added just
            // before the watchdog noticed stopping=true).
            int n = num_procs_.load(std::memory_order_acquire);
            for (int i = 1; i < n; ++i) {
                if (procs[i]) procs[i]->note.wake();
            }
            notify_watchers();

            for (int i = 1; i < n; ++i) {
                if (procs[i] && procs[i]->worker.joinable()) {
                    procs[i]->worker.join();
                }
            }

            procs.clear();
            num_procs_.store(0, std::memory_order_release);
            // Imp stacks may have been abandoned without destroy_imp; the
            // counters must not carry into the next epoch (🎯T36 / paper 34).
            live_gs.store(0, std::memory_order_release);
            daemon_gs.store(0, std::memory_order_release);
        }

        void Runtime::notify_watchers() {
            // Publish → fence → count read: pairs with park_wait's
            // register → fence → predicate read. TLA:ParkGate.NReadCount
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (park_waiters_.load(std::memory_order_relaxed) == 0) {
                return;
            }
            // Empty critical section: a watcher that evaluated its
            // predicate false still holds park_mu until cv.wait blocks;
            // serialize so the notify lands after it (same shape as the
            // shutdown fix, bug #8). TLA:ParkGate.NAcquireMu
            { std::lock_guard lk(park_mu); }
            park_cv.notify_all();
        }

        void Runtime::notify_quiesce_watchers() {
            // Same gate protocol, second instance (TLA:ParkGate) —
            // counts only watchers whose predicate reads parked state.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (quiesce_waiters_.load(std::memory_order_relaxed) == 0) {
                return;
            }
            { std::lock_guard lk(park_mu); }
            park_cv.notify_all();
        }

        void Runtime::broadcast_park() {
            { std::lock_guard lk(park_mu); }
            park_cv.notify_all();
        }

        void Runtime::unpark_one() {
            // Wake exactly one parked worker. Scan for a sleeping Note and CAS
            // SLEEPING->AWAKE + futex_wake on the first match. If no worker is
            // sleeping yet (all awake or in transition), flag one so it skips
            // its next sleep attempt. This eliminates the thundering herd of
            // notify_all on the worker path.
            // TLA:PerWorkerWake.SchedWakeSleeping TLA:PerWorkerWake.SchedFlagAwake
            int n = num_procs_.load(std::memory_order_acquire);
            // First pass: find a sleeping worker and wake exactly that one.
            for (int i = 1; i < n; ++i) {
                auto* p = procs[i].get();
                if (!p || !p->alive.load(std::memory_order_acquire)) continue;
                if (p->note.is_sleeping()) {
                    p->note.wake();
                    return;
                }
            }
            // Second pass: no sleeper found — flag one awake worker so it
            // won't sleep on its next park attempt.
            for (int i = 1; i < n; ++i) {
                auto* p = procs[i].get();
                if (!p || !p->alive.load(std::memory_order_acquire)) continue;
                if (p->parked.load(std::memory_order_acquire)) {
                    p->note.wake();
                    return;
                }
            }
            // All workers are active — no action needed.  park_cv
            // watchers are NOT notified here (🎯T34 O3): no watcher's
            // predicate becomes true because work arrived — main_loop
            // acts on completion (imp exit) and quiescence (worker
            // parks), each of which has its own notifier.
        }

        void Runtime::push_to_global(Imp* imp) {
            // Caller must hold global_mu and hold the placement claim.
            assert(!imp->next_);
            assert(!imp->in_global_);
            assert(imp->placed_.load(std::memory_order_relaxed));
            imp->in_global_ = true;
            global_run_queue.push_back(imp);
            has_global_work_.store(true, std::memory_order_release);
        }

        void Runtime::worker_loop() {
            auto& p = current_p();
            // TLA:PerWorkerWake.WorkerCheckWork
            while (!stopping.load(std::memory_order_acquire)) {
                p.heartbeat.fetch_add(1, std::memory_order_relaxed);

                // Try local run queue.
                Imp* next = local_next(p);
                if (next) {
                    next->run();
                    continue;
                }

                // Try global run queue.
                if (take_from_global(p)) {
                    continue;
                }

                // Try stealing from another processor's local queue.
                if (steal_work(p)) {
                    continue;
                }

                // Park: sleep on this worker's per-worker Note.
                //
                // Protocol (TLA:PerWorkerWake.WorkerSetParked / WorkerTrySleep):
                //   1. Set p.parked and notify park_cv so main_loop can observe
                //      quiescence (main_loop still waits on the shared park_cv).
                //   2. Sleep on p.note using a platform futex. This is a
                //      single-word futex with no shared condvar, so only one
                //      explicit unpark_one() call wakes exactly this worker.
                //   3. On wake: clear p.parked.
                //
                // Note: checking has_work() before sleeping in step 2 avoids a
                // TOCTOU window: if work arrived between steal_work() returning
                // false and p.parked being set, unpark_one() will see parked=true
                // and wake us, OR we see has_work() here and skip the sleep.
                p.parked.store(true, std::memory_order_release); // TLA:PerWorkerWake.WorkerSetParked
                // Wake main_loop / quiescent_loop's quiescence check.
                // Skipped entirely when no quiescence watcher is
                // registered (TLA:ParkGate) — this used to be a mutex +
                // broadcast syscall on every park.
                notify_quiesce_watchers();

                // Check for work that arrived after steal_work but before we
                // set parked. If found, skip the sleep (unpark_one may not have
                // been called because the work was added before parked=true).
                if (!stopping.load(std::memory_order_acquire) && !has_work(p)) {
                    // Surplus Ps wind down after 5s idle.
                    using namespace std::chrono;
                    constexpr auto wind_down = seconds(5);
                    bool is_surplus = p.id >= initial_procs_;

                    if (is_surplus) {
                        // TLA:PerWorkerWake.WorkerTrySleep (timed)
                        p.note.sleep_for(wind_down);
                    } else {
                        // TLA:PerWorkerWake.WorkerTrySleep TLA:PerWorkerWake.WorkerWoken
                        p.note.sleep();
                    }
                }

                p.parked.store(false, std::memory_order_release); // TLA:PerWorkerWake.WorkerClearParked

                // Surplus P wind-down: exit if still idle after timeout.
                if (p.id >= initial_procs_
                    && !stopping.load(std::memory_order_acquire)
                    && !has_work(p)) {
                    p.alive.store(false, std::memory_order_release);
                    // A dead P is skipped by all_parked scans, so this
                    // exit can complete quiescence — tell the watchers.
                    // (Latent before 🎯T34 O3: unpark_one's unconditional
                    // broadcasts papered over the missing notify here.)
                    notify_quiesce_watchers();
                    return;
                }
            }
        }

        void Runtime::main_loop() {
            auto user_done = [this] {
                return live_gs.load(std::memory_order_acquire)
                    <= daemon_gs.load(std::memory_order_acquire);
            };
            auto all_parked = [this] {
                int np = num_procs_.load(std::memory_order_acquire);
                for (int i = 1; i < np; ++i) {
                    if (procs[i] && procs[i]->alive.load(std::memory_order_acquire)
                        && !procs[i]->parked.load(std::memory_order_acquire))
                        return false;
                }
                return true;
            };

            for (;;) {
                // Register as a quiescence watcher only while a hook is
                // installed (fake_clock).  Without one, worker parks are
                // irrelevant here and must not wake this thread — and
                // work arriving never wakes it (nothing to do with work;
                // workers are unparked directly via their Notes).
                bool const hook = has_hook_.load(std::memory_order_acquire);
                park_wait([&] {
                    if (user_done()) return true;
                    // Hook installed/uninstalled while we waited
                    // (broadcast_park wakes us unconditionally): loop
                    // around and re-register with fresh parameters.
                    if (has_hook_.load(std::memory_order_acquire) != hook) return true;
                    // Only wake for quiescence when a hook is registered
                    // (e.g. fake_clock). Without a hook there is nothing
                    // useful to do when all workers are parked — sleeping
                    // is correct; busy-spinning is not.
                    if (hook && all_parked()) return true;
                    return false;
                }, /*quiescence=*/hook);
                if (user_done()) break;
                if (has_global_work_.load(std::memory_order_acquire)) continue;
                // Quiescent: all workers parked. Call hook if registered.
                {
                    std::lock_guard hlk(hook_mu_);
                    if (quiescence_hook_) {
                        if (!quiescence_hook_()) {
                            // Hook has no more fake-clock work. Live imps
                            // woken by the last timer fire may not have run
                            // to completion yet — only exit if truly done.
                            if (user_done()) break;
                            // Re-park; workers will drain remaining imps.
                        }
                        continue;
                    }
                }
                // No hook — genuine deadlock or waiting for external event.
                // Park again; we'll wake on global work or user_done.
                continue;
            }
        }

        void Runtime::quiescent_loop() {
            // Wait until no runnable work remains (all workers parked and
            // global queue empty).  Unlike main_loop(), suspended imps that
            // are waiting for external events (e.g. fake_clock timers) do NOT
            // prevent this from returning — they are not in any run queue.
            park_wait([this] {
                if (has_global_work_.load(std::memory_order_acquire)) {
                    return false;
                }
                int n = num_procs_.load(std::memory_order_acquire);
                for (int i = 0; i < n; ++i) {
                    auto& p = *procs[i];
                    if (!p.alive.load(std::memory_order_acquire)) continue;
                    if (!p.parked.load(std::memory_order_acquire)) return false;
                }
                return true;
            }, /*quiescence=*/true);
        }

        void Runtime::watchdog_loop() {
            using namespace std::chrono;
            constexpr auto interval = milliseconds(10);
            // A worker is "stalled" only after this many consecutive
            // ticks with no heartbeat progress (100 ms). One missed tick
            // means "mid-slice", not "blocked": a healthy worker whose
            // slice runs 15-50 ms (routine under TSan's 10-20× slowdown,
            // or any compute-heavy imp) is indistinguishable from a
            // blocked one at single-tick granularity, and treating it as
            // stalled grew the pool to max_procs_ on every TSan run
            // (paper 32 addendum). The watchdog rescues *blocked*
            // workers; 100 ms detection latency is still far below the
            // wind-down and test timescales that depend on rescue.
            constexpr int kStallTicks = 10;

            std::vector<uint64_t> last(max_procs_, 0);
            std::vector<int> stalled_ticks(max_procs_, 0);

            while (!stopping.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(interval);

                int n = num_procs_.load(std::memory_order_acquire);
                // Skip P0: it runs main_loop(), not worker_loop(), so its
                // heartbeat never increments.  Monitoring it would trigger
                // spurious add_processor() calls that flood max_procs_.
                for (int i = 1; i < n; ++i) {
                    auto& p = *procs[i];
                    if (!p.alive.load(std::memory_order_acquire)) continue;
                    if (p.parked.load(std::memory_order_acquire)) {
                        last[i] = p.heartbeat.load(std::memory_order_acquire);
                        stalled_ticks[i] = 0;
                        continue;
                    }
                    uint64_t hb = p.heartbeat.load(std::memory_order_acquire);
                    if (hb != last[i]) {
                        stalled_ticks[i] = 0;
                    } else if (++stalled_ticks[i] >= kStallTicks) {
                        // Re-arm: rescue again only after another full
                        // window of continued no-progress.
                        stalled_ticks[i] = 0;
                        // P is stalled — but only add a rescue P when there
                        // is work a new P could actually take: a queued MT
                        // in the stalled P's ring (beyond the sentinel and
                        // the imp that is hogging the P), or global work.
                        // A stalled P with nothing queued behind it (one imp
                        // in a long compute stretch or blocking syscall)
                        // needs no rescue; unconditionally adding one churns
                        // add→park→wind-down→die→re-add every tick. Under
                        // TSan's 10-20× slowdown that churn saturated
                        // max_procs_ (64 procs, ~64k thread creations per
                        // suite run — paper 32, C1).
                        bool rescuable = false;
                        {
                            std::lock_guard lk(p.run_mu);
                            if (auto* start = p.busy) {
                                auto* it = start;
                                do {
                                    if (it != &p.main && it != p.running) {
                                        rescuable = true;
                                        break;
                                    }
                                    it = it->next_;
                                } while (it != start);
                            }
                        }
                        if (!rescuable) {
                            rescuable = has_global_work_.load(std::memory_order_acquire);
                        }
                        if (rescuable) {
                            // Reuse before add: a woken parked P steals
                            // exactly like a fresh P would, and re-using
                            // the pool prevents the second churn shape —
                            // rescue P finishes, parks, and the next tick
                            // ADDS another instead of waking it. Only
                            // create a new P when nobody is parked (the
                            // mn watchdog stress tests' regime).
                            if (!try_wake_parked_worker()) {
                                add_processor();
                            }
                        }
                    }
                    last[i] = hb;
                }
            }
        }

        bool Runtime::try_wake_parked_worker() {
            // Same two-pass shape as unpark_one(), but reports success and
            // skips the park_cv notifies — the watchdog isn't publishing
            // new work, just redirecting an idle P at work that already
            // exists (which main_loop's predicates track independently).
            int n = num_procs_.load(std::memory_order_acquire);
            for (int i = 1; i < n; ++i) {
                auto* p = procs[i].get();
                if (!p || !p->alive.load(std::memory_order_acquire)) continue;
                if (p->note.is_sleeping()) {
                    p->note.wake();
                    return true;
                }
            }
            for (int i = 1; i < n; ++i) {
                auto* p = procs[i].get();
                if (!p || !p->alive.load(std::memory_order_acquire)) continue;
                if (p->parked.load(std::memory_order_acquire)) {
                    p->note.wake();
                    return true;
                }
            }
            return false;
        }

        void Runtime::add_processor() {
            std::lock_guard lk(global_mu);
            int n = num_procs_.load(std::memory_order_relaxed);

            // Try to reuse a dead surplus slot.
            int idx = -1;
            for (int i = initial_procs_; i < n; ++i) {
                if (procs[i] && !procs[i]->alive.load(std::memory_order_acquire)) {
                    if (procs[i]->worker.joinable()) {
                        procs[i]->worker.join();
                    }
                    idx = i;
                    break;
                }
            }

            if (idx >= 0) {
                // Reset in-place to avoid racing with steal_work
                // on the unique_ptr.
                procs[idx]->reset();
            } else {
                // No reusable slot — allocate a new one.
                if (n >= max_procs_) return;
                idx = n;
                procs[idx] = std::make_unique<Processor>(idx);
                num_procs_.store(n + 1, std::memory_order_release);
                note_procs_high_water(n + 1);
            }
            procs[idx]->worker = std::thread([this, idx] {
                set_thread_name(idx);
                bind_processor(procs[idx].get());
                worker_loop();
            });

            // Wake existing workers so they notice the new P for stealing.
            unpark_one();
        }

        // TLA:StealWork.VLocalNext
        Imp* Runtime::local_next(Processor& p) {
            std::lock_guard lk(p.run_mu);
            auto& busy = p.busy;
            if (!busy) {
                p.running = nullptr;
                return nullptr;
            }

            // Skip past the main imp (sentinel) to find real work.
            auto* ci = current_imp();
            auto* candidate = busy;
            if (candidate == ci) {
                candidate = candidate->next_;
            }
            if (candidate == ci || candidate == &p.main) {
                p.running = nullptr;
                return nullptr;
            }
            // Mark this MT as claimed so steal_work on other Ps skips it.
            p.running = candidate;
            return candidate;
        }

        // TLA:StealWork.TkAcquireGlobal TLA:StealWork.TkPopAndSchedule
        bool Runtime::take_from_global([[maybe_unused]] Processor& p) {
            std::lock_guard lk(global_mu);
            if (global_run_queue.empty()) {
                has_global_work_.store(false, std::memory_order_release);
                return false;
            }

            // Take a fair share so other workers also get work.
            int avail = (int)global_run_queue.size();
            int np = num_procs_.load(std::memory_order_relaxed);
            int n = std::max(1, avail / np);
            for (int i = 0; i < n; ++i) {
                auto* imp = global_run_queue.front();
                global_run_queue.pop_front();
                imp->in_global_ = false;
                imp->schedule_local();
            }
            if (global_run_queue.empty()) {
                has_global_work_.store(false, std::memory_order_release);
            }
            return true;
        }

        bool Runtime::steal_work(Processor& thief) {
            int n = num_procs_.load(std::memory_order_acquire);
            for (int i = 0; i < n; ++i) {
                auto& victim = *procs[i];
                if (&victim == &thief) continue;
                if (!victim.alive.load(std::memory_order_acquire)) continue;

                Imp* stolen = nullptr;
                {
                    std::lock_guard lk(victim.run_mu); // TLA:StealWork.TStealAcquireRunMu

                    // Try to acquire global_mu without blocking to avoid
                    // deadlock (take_from_global holds global_mu then
                    // acquires run_mu via schedule_local).
                    std::unique_lock glk(global_mu, std::try_to_lock); // TLA:StealWork.TStealTryGlobalOK TLA:StealWork.TStealTryGlobalFail
                    if (!glk) continue;

                    if (!victim.busy) continue;

                    // TLA:StealWork.TStealCheck
                    auto* candidate = victim.busy->prev_;
                    if (!candidate || candidate == &victim.main
                        || candidate == victim.busy
                        || candidate == victim.running) {
                        continue; // TLA:StealWork.TStealReleaseFail
                    }

                    // TLA:StealWork.TStealDelinkPush
                    // Delink from victim's DLL and push to global
                    // atomically (both locks held) so schedule() cannot
                    // see the MT with next_==null / in_global_==false.
                    candidate->prev_->next_ = candidate->next_;
                    candidate->next_->prev_ = candidate->prev_;
                    candidate->next_ = nullptr;
                    candidate->prev_ = nullptr;
                    push_to_global(candidate);
                    stolen = candidate;
                } // TLA:StealWork.TStealReleaseOK
                if (stolen) {
                    unpark_one();
                    return true;
                }
            }
            return false;
        }

        bool Runtime::has_work(Processor& p) {
            {
                std::lock_guard lk(p.run_mu);
                // Queue has real work if there's more than just the sentinel.
                if (p.busy && p.busy->next_ != p.busy) {
                    return true;
                }
            }

            {
                std::lock_guard lk(global_mu);
                if (!global_run_queue.empty()) {
                    return true;
                }
            }

            return false;
        }

    }

}
