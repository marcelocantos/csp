#include <csp/internal/runtime.h>

#include <algorithm>
#include <cassert>

namespace csp {

    namespace detail {

        static Runtime g_runtime;

        Runtime& Runtime::instance() {
            return g_runtime;
        }

        void Runtime::init(int num_procs) {
            // Shut down any previous state.
            if (!procs.empty()) {
                shutdown();
            }

            stopping.store(false, std::memory_order_release);
            live_gs.store(0, std::memory_order_release);

            {
                std::lock_guard<std::mutex> lk(global_mu);
                global_run_queue.clear();
            }

            if (num_procs <= 0) {
                num_procs = std::max(1, (int)std::thread::hardware_concurrency());
            }

            procs.reserve(num_procs);
            for (int i = 0; i < num_procs; ++i) {
                procs.push_back(std::make_unique<Processor>(i));
            }
            bind_processor(procs[0].get());

            for (int i = 1; i < num_procs; ++i) {
                workers.emplace_back([this, i] {
                    bind_processor(procs[i].get());
                    worker_loop();
                });
            }
        }

        void Runtime::shutdown() {
            stopping.store(true, std::memory_order_release);
            // Acquire-release park_mu to synchronize with workers'
            // park_cv.wait() — ensures any worker that has already
            // checked the predicate (seeing stopping==false) has
            // entered wait() before we notify, preventing lost
            // notifications.
            { std::lock_guard<std::mutex> lk(park_mu); }
            park_cv.notify_all();

            for (auto& w : workers) {
                if (w.joinable()) {
                    w.join();
                }
            }

            workers.clear();
            procs.clear();
        }

        void Runtime::unpark_one() {
            park_cv.notify_all();
        }

        void Runtime::push_to_global(Microthread* mt) {
            // Caller must hold global_mu.
            assert(!mt->next_);
            assert(!mt->in_global_);
            mt->in_global_ = true;
            global_run_queue.push_back(mt);
        }

        void Runtime::worker_loop() {
            auto& p = current_p();

            while (!stopping.load(std::memory_order_acquire)) {
                // Fire expired timers.
                fire_timers(p);

                // Try local run queue.
                Microthread* next = local_next(p);
                if (next) {
                    next->run();
                    continue;
                }

                // Try global run queue.
                if (take_from_global(p)) {
                    continue;
                }

                // Park: wait for work or shutdown.
                {
                    std::unique_lock<std::mutex> lk(park_mu);
                    p.parked.store(true, std::memory_order_release);

                    auto deadline = next_timer_deadline(p);
                    if (deadline) {
                        park_cv.wait_until(lk, *deadline, [this, &p] {
                            return stopping.load(std::memory_order_acquire)
                                || has_work(p);
                        });
                    } else {
                        park_cv.wait(lk, [this, &p] {
                            return stopping.load(std::memory_order_acquire)
                                || has_work(p);
                        });
                    }

                    p.parked.store(false, std::memory_order_release);
                }
            }
        }

        void Runtime::main_loop() {
            // Main thread waits for all microthreads to complete.
            // Workers do all the actual execution.
            std::unique_lock<std::mutex> lk(park_mu);
            park_cv.wait(lk, [this] {
                return live_gs.load(std::memory_order_acquire) == 0;
            });
        }

        Microthread* Runtime::local_next(Processor& p) {
            std::lock_guard<std::mutex> lk(p.run_mu);
            auto& busy = p.busy;
            if (!busy) return nullptr;

            // Skip past g_self (the sentinel/main) to find real work.
            auto* candidate = busy;
            if (candidate == g_self) {
                candidate = candidate->next_;
            }
            if (candidate == g_self || candidate == &p.main) {
                return nullptr;
            }
            return candidate;
        }

        bool Runtime::take_from_global(Processor& p) {
            std::lock_guard<std::mutex> lk(global_mu);
            if (global_run_queue.empty()) {
                return false;
            }

            // Take a fair share so other workers also get work.
            int avail = (int)global_run_queue.size();
            int n = std::max(1, avail / (int)procs.size());
            for (int i = 0; i < n; ++i) {
                auto* mt = global_run_queue.front();
                global_run_queue.pop_front();
                mt->in_global_ = false;
                mt->schedule_local();
            }
            return true;
        }

        void Runtime::fire_timers(Processor& p) {
            auto now = std::chrono::steady_clock::now();
            while (!p.timer_heap.empty() && p.timer_heap.top().deadline <= now) {
                auto* mt = p.timer_heap.top().thread;
                p.timer_heap.pop();
                mt->schedule_local();
            }
        }

        bool Runtime::steal_work(Processor& thief) {
            for (auto& victim_ptr : procs) {
                auto& victim = *victim_ptr;
                if (&victim == &thief) continue;

                Microthread* stolen = nullptr;
                {
                    std::lock_guard<std::mutex> lk(victim.run_mu);
                    if (!victim.busy) continue;

                    auto* candidate = victim.busy->prev_;
                    if (!candidate || candidate == &victim.main || candidate == victim.busy) {
                        continue;
                    }

                    candidate->prev_->next_ = candidate->next_;
                    candidate->next_->prev_ = candidate->prev_;
                    candidate->next_ = nullptr;
                    candidate->prev_ = nullptr;
                    stolen = candidate;
                }
                // victim.run_mu released — safe to schedule on thief.
                if (stolen) {
                    stolen->schedule();
                    return true;
                }
            }
            return false;
        }

        std::optional<std::chrono::steady_clock::time_point>
        Runtime::next_timer_deadline(Processor& p) {
            if (p.timer_heap.empty()) {
                return std::nullopt;
            }
            return p.timer_heap.top().deadline;
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

            if (!p.timer_heap.empty() &&
                p.timer_heap.top().deadline <= std::chrono::steady_clock::now()) {
                return true;
            }

            return false;
        }

    }

}
