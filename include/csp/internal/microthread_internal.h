#ifndef INCLUDED__csp__microthread_internal_h
#define INCLUDED__csp__microthread_internal_h

#include <csp/microthread.h>
#include <csp/fcontext.h>

#include <atomic>
#include <cstdlib>
#include <cstddef>

using namespace csp;

namespace csp {

    namespace detail {

        enum class Status : intptr_t { run, sleep, detach, exit, spawn };

        struct Microthread;

        extern thread_local Microthread * g_self;

        void do_switch(Status status = Status::sleep);

        struct alignas(16) Microthread {
            struct alignas(16) StackSlot { char c[16]; };

            static constexpr size_t stack_size = 32 << 10;

            Microthread * prev_;
            Microthread * next_;
            std::atomic<fcontext_t> ctx_;
            StackSlot * stk_;
            char status_[32];
            csp_chanop const * chanops_;
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

#endif // INCLUDED__csp__microthread_internal_h
