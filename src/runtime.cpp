#include <csp/internal/runtime.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <csignal>
#endif

#include <algorithm>
#include <cassert>
#include <cstdio>

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
            stopping.store(true, std::memory_order_release); // TLA:WorkerParking.ShutdownSetFlag
            // Acquire-release park_mu to synchronize with workers'
            // park_cv.wait() — ensures any worker that has already
            // checked the predicate (seeing stopping==false) has
            // entered wait() before we notify, preventing lost
            // notifications.
            { std::lock_guard<std::mutex> lk(park_mu); } // TLA:WorkerParking.ShutdownAcquireMu TLA:WorkerParking.ShutdownReleaseMu
            park_cv.notify_all(); // TLA:WorkerParking.ShutdownNotify

            if (watchdog_.joinable()) {
                watchdog_.join();
            }

            int n = num_procs_.load(std::memory_order_acquire);
            for (int i = 1; i < n; ++i) {
                if (procs[i] && procs[i]->worker.joinable()) {
                    procs[i]->worker.join();
                }
            }

            procs.clear();
            num_procs_.store(0, std::memory_order_release);
        }

        void Runtime::unpark_one() {
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
            // TLA:WorkerParking.WorkerCheckWork
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

                // Park: wait for work or shutdown.
                {
                    std::unique_lock<std::mutex> lk(park_mu); // TLA:WorkerParking.WorkerAcquirePark
                    p.parked.store(true, std::memory_order_release);
                    park_cv.notify_all();  // wake run() quiescence check

                    // Surplus Ps wind down after 5s idle.
                    using namespace std::chrono;
                    constexpr auto wind_down = seconds(5);
                    bool is_surplus = p.id >= initial_procs_;

                    if (is_surplus) {
                        park_cv.wait_until(lk, steady_clock::now() + wind_down, [this, &p] {
                            return stopping.load(std::memory_order_acquire)
                                || has_work(p);
                        });
                    } else {
                        // TLA:WorkerParking.WorkerEvalPred TLA:WorkerParking.WorkerEnterWait TLA:WorkerParking.WorkerWoken
                        park_cv.wait(lk, [this, &p] {
                            return stopping.load(std::memory_order_acquire)
                                || has_work(p);
                        });
                    }

                    p.parked.store(false, std::memory_order_release); // TLA:WorkerParking.WorkerWake
                }

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
            // Workers handle all execution; main thread just waits
            // until all non-daemon imps have exited.
            std::unique_lock<std::mutex> lk(park_mu);
            park_cv.wait(lk, [this] {
                return live_gs.load(std::memory_order_acquire)
                    <= daemon_gs.load(std::memory_order_acquire);
            });
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
                for (int i = 0; i < n; ++i) {
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
        bool Runtime::take_from_global(Processor& p) {
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
