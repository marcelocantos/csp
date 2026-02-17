#pragma once

#include <csp/csp.h>
#include <csp/fcontext.h>

#include <atomic>
#include <cstdlib>
#include <cstddef>

// TSan fiber annotations: tell TSan about user-mode context switches
// so it can correctly track happens-before across microthread switches.
#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))
#define CSP_TSAN 1
extern "C" {
    void *__tsan_get_current_fiber(void);
    void *__tsan_create_fiber(unsigned flags);
    void __tsan_destroy_fiber(void *fiber);
    void __tsan_switch_to_fiber(void *fiber, unsigned flags);
}
#endif

using namespace csp;

namespace csp {

    namespace detail {

        enum class Status : intptr_t { run, sleep, detach, exit, spawn };

        struct Microthread;

        extern thread_local Microthread * g_self;

        void do_switch(Status status = Status::sleep);

        struct alignas(16) Microthread {
            struct alignas(16) StackSlot { char c[16]; };

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    (defined(__has_feature) && (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer)))
            static constexpr size_t stack_size = 128 << 10;  // sanitizers need ~4x headroom
#else
            static constexpr size_t stack_size = 32 << 10;
#endif

            Microthread * prev_;
            Microthread * next_;
            std::atomic<fcontext_t> ctx_;
            StackSlot * stk_;
            char status_[32];
            internal::ChanOp const * chanops_;
            int n_chanops_, signal_;

            size_t id_ = []{
                static std::atomic<size_t> next_{0};
                return next_++;
            }();

            Microthread(fcontext_t ctx, StackSlot * stk);
            Microthread();
            Microthread(Microthread const &) = delete;

            Microthread & operator=(Microthread const &) = delete;

            char const * getfullstatus_() const {
                return status_;
            }

            void schedule(bool make_current = false);
            void schedule_local(bool make_current = false);
            void deschedule();

            void run(Status status = Status::sleep);

            enum AltState : uint32_t { ALT_IDLE, ALT_WAITING, ALT_CLAIMED };
            std::atomic<uint32_t> alt_state{ALT_IDLE};

            bool in_global_ = false;  // true while in the global run queue
            std::atomic<bool> wake_pending_{false};  // set by schedule() during suspending_ window
            std::atomic<bool> suspending_{false};  // true from unlock_all to do_switch completion

#if CSP_TSAN
            void* tsan_fiber_ = nullptr;  // TSan fiber handle for this microthread
#endif
        };

        inline
        char const * getfullstatus(Microthread const * mt) {
            return mt ? mt->getfullstatus_() : "Ø";
        }

        inline
        char const * getstatus(Microthread const * mt) {
            return getfullstatus(mt);
        }

    }

}
