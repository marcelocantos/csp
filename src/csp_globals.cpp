#include <csp/internal/blocking_pool.h>
#include <csp/internal/reactor.h>
#include <csp/internal/runtime.h>
#include <csp/internal/stack_pool.h>

#include <cstdio>

namespace csp {

    static struct StaticInitDiag {
        StaticInitDiag() { fprintf(stderr, "CSP_DIAG: csp_globals static init begin\n"); fflush(stderr); }
    } s_diag;

    writer<std::exception_ptr> global_exception_handler = std::move(chan<std::exception_ptr>().w);

    static struct StaticInitDiag2 {
        StaticInitDiag2() { fprintf(stderr, "CSP_DIAG: global_exception_handler OK\n"); fflush(stderr); }
    } s_diag2;

    poke_t poke;

    reader<> const skip = std::move(chan<>().r);

    static struct StaticInitDiag3 {
        StaticInitDiag3() { fprintf(stderr, "CSP_DIAG: skip OK, csp_globals init done\n"); fflush(stderr); }
    } s_diag3;

    namespace detail {

        static thread_local Imp * g_imp = nullptr;

        Imp* current_imp() { return g_imp; }
        void set_current_imp(Imp* p) { g_imp = p; }

        static thread_local Processor * tl_proc_ = nullptr;
        static bool runtime_initialized_ = false;

        Processor& current_p() {
            if (!tl_proc_) {
                if (!runtime_initialized_) {
                    Runtime::instance().init(1);
                    runtime_initialized_ = true;
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

    void init_runtime(int num_procs) {
        auto& rt = detail::Runtime::instance();
        rt.init(num_procs);
        detail::runtime_initialized_ = true;

        if (num_procs != 1) {
            set_scheduler([&rt] {
                rt.main_loop();
            });
        }
    }

    void shutdown_runtime() {
        detail::BlockingPool::instance().shutdown();
        detail::Reactor::instance().shutdown();
        detail::Runtime::instance().shutdown();
        detail::StackPool::instance().drain();
        detail::runtime_initialized_ = false;
        detail::tl_proc_ = nullptr;

        // Restore default single-threaded scheduler.
        reset_scheduler();
    }

}
