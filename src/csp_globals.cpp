#include <csp/internal/blocking_pool.h>
#include <csp/internal/reactor.h>
#include <csp/internal/runtime.h>
#include <csp/internal/stack_pool.h>

#include <cstdlib>

namespace csp {

    writer<std::exception_ptr> global_exception_handler = std::move(chan<std::exception_ptr>().w);

    poke_t poke;

    reader<> const skip = std::move(chan<>().r);

    namespace detail {

        static thread_local Imp * g_imp = nullptr;

        Imp* current_imp() { return g_imp; }
        void set_current_imp(Imp* p) { g_imp = p; }

        static thread_local Processor * tl_proc_ = nullptr;
        static bool runtime_initialized_ = false;

        // -1 = unset (read CSP_MAXPROCS at init time), 0 = auto, >0 = explicit
        static int g_maxprocs = -1;

        static int resolve_maxprocs() {
            int n = g_maxprocs;
            if (n < 0) {
                // Read from environment.
                if (auto* env = std::getenv("CSP_MAXPROCS")) {
                    n = std::atoi(env);
                    if (n < 0) n = 0;
                } else {
                    n = 0; // auto (hardware concurrency)
                }
            }
            return n;
        }

        static void init_runtime(int num_procs) {
            auto& rt = Runtime::instance();
            rt.init(num_procs);
            runtime_initialized_ = true;

            if (num_procs != 1) {
                set_scheduler([&rt] {
                    rt.main_loop();
                });
            }

        }

        Processor& current_p() {
            if (!tl_proc_) {
                if (!runtime_initialized_) {
                    init_runtime(resolve_maxprocs());
                } else {
                    // Worker thread — bind_processor should have been called.
                    std::terminate();
                }
            }
            return *tl_proc_;
        }

        bool has_processor() {
            return tl_proc_ != nullptr;
        }

        void bind_processor(Processor * p) {
            tl_proc_ = p;
            g_imp = &p->main;
#if CSP_TSAN
            p->main.tsan_fiber_ = __tsan_get_current_fiber();
#endif
        }

    }

    void set_maxprocs(int n) {
        if (n < 0) n = 0;
        if (n == 0) {
            n = std::max(1, (int)std::thread::hardware_concurrency());
        }
        detail::g_maxprocs = n;

        if (!detail::runtime_initialized_) return;

        auto& rt = detail::Runtime::instance();

        // Enable M:N scheduler if we just went from 1→N.
        if (n > 1 && !rt.mn_mode_) {
            rt.mn_mode_ = true;
            set_scheduler([&rt] { rt.main_loop(); });
        }

        // Update initial_procs_. The watchdog adds P's when it detects
        // stalls and num_procs_ < max_procs_; surplus P's (id >=
        // initial_procs_) wind down after an idle timeout.
        bool scaling_down = n < rt.initial_procs_;
        rt.initial_procs_ = n;
        if (scaling_down) {
            // Wake parked P's so they re-evaluate surplus status.
            rt.park_cv.notify_all();
        }
    }

    void shutdown_runtime() {
        detail::BlockingPool::instance().shutdown();
        detail::Reactor::instance().shutdown();
        detail::Runtime::instance().shutdown();
        detail::StackPool::instance().drain();
        detail::runtime_initialized_ = false;
        detail::g_maxprocs = -1;
        detail::tl_proc_ = nullptr;

        // Restore default single-threaded scheduler.
        reset_scheduler();
    }

}
