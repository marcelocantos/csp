#include <csp/internal/runtime.h>

namespace csp {

    writer<std::exception_ptr> global_exception_handler = ++channel<std::exception_ptr>{};

    poke_t poke;

    reader<> const skip = --channel<>();

    namespace detail {

        thread_local Microthread * g_self = nullptr;

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

        void bind_processor(Processor * p) {
            tl_proc_ = p;
            g_self = &p->main;
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
        detail::Runtime::instance().shutdown();
        detail::runtime_initialized_ = false;
        detail::tl_proc_ = nullptr;

        // Restore default single-threaded scheduler.
        set_scheduler([]{ while (csp_run()) { } });
    }

}
