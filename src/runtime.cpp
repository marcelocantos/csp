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
                fprintf(stderr, "CSP_PROC_HIGH_WATER=%d\n",
                        g_procs_high_water.load(std::memory_order_relaxed));
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

            {
                std::lock_guard<std::mutex> lk(global_mu);
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
            // we notify. This prevents lost wakeups. // TLA:WorkerParking.ShutdownAcquireMu
            { std::lock_guard<std::mutex> lk(park_mu); } // TLA:WorkerParking.ShutdownReleaseMu
            park_cv.notify_all(); // TLA:WorkerParking.ShutdownNotify

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
            { std::lock_guard<std::mutex> lk(park_mu); }
            park_cv.notify_all();

            for (int i = 1; i < n; ++i) {
                if (procs[i] && procs[i]->worker.joinable()) {
                    procs[i]->worker.join();
                }
            }

            procs.clear();
            num_procs_.store(0, std::memory_order_release);
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
                    // Also wake main_loop / run() / quiescent_loop which wait
                    // on the shared park_cv. They use park_cv.wait with
                    // has_global_work_ in the predicate and need notification
                    // when the scheduler adds work.
                    park_cv.notify_all();
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
                    park_cv.notify_all();
                    return;
                }
            }
            // All workers are active — no action needed.
            // Still notify park_cv: main_loop checks has_global_work_ and
            // needs to be woken when work is pushed to the global queue.
            park_cv.notify_all();
        }

        void Runtime::push_to_global(Imp* imp) {
            // Caller must hold global_mu.
            assert(!imp->next_);
            assert(!imp->in_global_);
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
                {
                    // Notify main_loop / quiescent_loop while holding park_mu
                    // so they see a consistent parked snapshot.
                    std::lock_guard<std::mutex> lk(park_mu);
                    park_cv.notify_all();  // wake main_loop quiescence check
                }

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
                {
                    std::unique_lock<std::mutex> lk(park_mu);
                    park_cv.wait(lk, [&] {
                        if (user_done()) return true;
                        if (has_global_work_.load(std::memory_order_acquire)) return true;
                        // Only wake for quiescence when a hook is registered
                        // (e.g. fake_clock). Without a hook there is nothing
                        // useful to do when all workers are parked — sleeping
                        // is correct; busy-spinning is not.
                        if (has_hook_.load(std::memory_order_acquire) && all_parked()) return true;
                        return false;
                    });
                }
                if (user_done()) break;
                if (has_global_work_.load(std::memory_order_acquire)) continue;
                // Quiescent: all workers parked. Call hook if registered.
                {
                    std::lock_guard<std::mutex> hlk(hook_mu_);
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
            std::unique_lock<std::mutex> lk(park_mu);
            park_cv.wait(lk, [this] {
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
            });
        }

        void Runtime::watchdog_loop() {
            using namespace std::chrono;
            constexpr auto interval = milliseconds(10);

            std::vector<uint64_t> last(max_procs_, 0);

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
                        continue;
                    }
                    uint64_t hb = p.heartbeat.load(std::memory_order_acquire);
                    if (hb == last[i]) {
                        // P is stalled — add a new P so work stealing
                        // can drain its queue.
                        add_processor();
                    }
                    last[i] = hb;
                }
            }
        }

        void Runtime::add_processor() {
            std::lock_guard<std::mutex> lk(global_mu);
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
            std::lock_guard<std::mutex> lk(p.run_mu);
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
            std::lock_guard<std::mutex> lk(global_mu);
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
                    std::lock_guard<std::mutex> lk(victim.run_mu); // TLA:StealWork.TStealAcquireRunMu

                    // Try to acquire global_mu without blocking to avoid
                    // deadlock (take_from_global holds global_mu then
                    // acquires run_mu via schedule_local).
                    std::unique_lock<std::mutex> glk(global_mu, std::try_to_lock); // TLA:StealWork.TStealTryGlobalOK TLA:StealWork.TStealTryGlobalFail
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
                std::lock_guard<std::mutex> lk(p.run_mu);
                // Queue has real work if there's more than just the sentinel.
                if (p.busy && p.busy->next_ != p.busy) {
                    return true;
                }
            }

            {
                std::lock_guard<std::mutex> lk(global_mu);
                if (!global_run_queue.empty()) {
                    return true;
                }
            }

            return false;
        }

    }

}
